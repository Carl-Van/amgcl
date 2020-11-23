#include <vector>
#include <iostream>

#include <amgcl/backend/builtin.hpp>
#include <amgcl/value_type/static_matrix.hpp>
#include <amgcl/adapter/crs_tuple.hpp>
#include <amgcl/adapter/block_matrix.hpp>

#include <amgcl/mpi/distributed_matrix.hpp>
#include <amgcl/mpi/make_solver.hpp>
#include <amgcl/mpi/amg.hpp>
#include <amgcl/mpi/coarsening/smoothed_aggregation.hpp>
#include <amgcl/mpi/relaxation/spai0.hpp>
#include <amgcl/mpi/solver/bicgstab.hpp>

#include <amgcl/io/binary.hpp>
#include <amgcl/profiler.hpp>

// Block size
const int B = 3;

int main(int argc, char *argv[]) {
    // The command line should contain the matrix file name:
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <matrix.bin>" << std::endl;
        return 1;
    }

    amgcl::mpi::init mpi(&argc, &argv);
    amgcl::mpi::communicator world(MPI_COMM_WORLD);

    // The profiler:
    amgcl::profiler<> prof("Serena MPI");

    prof.tic("read");
    // Get the global size of the matrix:
    size_t rows = amgcl::io::crs_size<size_t>(argv[1]);

    if (world.rank == 0) std::cout
        << "World size: " << world.size << std::endl
        << "Matrix " << argv[1] << ": " << rows << " rows" << std::endl;

    // Split the matrix into approximately equal chunks of rows, so that each
    // chunk size is still divisible by the block size.
    size_t chunk = (rows + world.size - 1) / world.size;
    if (chunk % B) chunk += B - chunk % B;
    size_t row_beg = std::min(rows, chunk * world.rank);
    size_t row_end = std::min(rows, row_beg + chunk);
    chunk = row_end - row_beg;

    // Read our part of the system matrix.
    std::vector<ptrdiff_t> ptr, col;
    std::vector<double> val;
    amgcl::io::read_crs(argv[1], rows, ptr, col, val, row_beg, row_end);
    prof.toc("read");

    // Declare the backend and the solver types
    typedef amgcl::static_matrix<double, B, B> dmat_type;
    typedef amgcl::static_matrix<double, B, 1> dvec_type;
    typedef amgcl::static_matrix<float,  B, B> fmat_type;
    typedef amgcl::backend::builtin<dmat_type> DBackend;
    typedef amgcl::backend::builtin<fmat_type> FBackend;

    typedef amgcl::mpi::make_solver<
        amgcl::mpi::amg<
            FBackend,
            amgcl::mpi::coarsening::smoothed_aggregation<FBackend>,
            amgcl::mpi::relaxation::spai0<FBackend>,
            amgcl::mpi::direct::skyline_lu<fmat_type>
            >,
        amgcl::mpi::solver::bicgstab<DBackend>
        > Solver;

    // We need to scale the matrix, so that it has the unit diagonal.
    // Since we only have the local rows for the matrix, and we may need the
    // remote diagonal values, it is more convenient to represent the scaling
    // with the matrix-matrix product (As = D^-1/2 A D^-1/2).

    prof.tic("scale");
    // Find the local diagonal values,
    // and form the CRS arrays for a diagonal matrix.
    std::vector<double> dia(chunk, 1.0);
    std::vector<ptrdiff_t> d_ptr(chunk + 1), d_col(chunk);
    for(size_t i = 0, I = row_beg; i < chunk; ++i, ++I) {
        d_ptr[i] = i;
        d_col[i] = I;
        for(ptrdiff_t j = ptr[i], e = ptr[i+1]; j < e; ++j) {
            if (col[j] == I) {
                dia[i] = 1 / sqrt(val[j]);
                break;
            }
        }
    }
    d_ptr.back() = chunk;

    // Create amgcl distributed matrix using the local diagonal parts.
    amgcl::mpi::distributed_matrix<DBackend> D(world,
            amgcl::adapter::block_matrix<dmat_type>(
                std::tie(chunk, d_ptr, d_col, dia)));

    // The scaled matrix is formed as product D * A * D,
    // where A is the local matrix part converted to the block format on the fly.
    auto Ad = product(D, *product(
                amgcl::mpi::distributed_matrix<DBackend>(world,
                    amgcl::adapter::block_matrix<dmat_type>(
                        std::tie(chunk, ptr, col, val))),
                D));

    // In order to setup the preconditioner, we need the same matrix in single
    // precision. We do this by explicitly converting the local and the remote
    // parts of the double-precision matrix.
    auto Af = std::make_shared<amgcl::mpi::distributed_matrix<FBackend>>(
            world,
            std::make_shared<amgcl::backend::crs<fmat_type>>(*Ad->local()),
            std::make_shared<amgcl::backend::crs<fmat_type>>(*Ad->remote())
            );
    prof.toc("scale");

    prof.tic("setup");
    Solver::params prm;
    prm.solver.maxiter = 200;

    // We now may move the double-precision matrix to the backend.
    // This in general moves the internal data structures to an opaque backend
    // format and we no longer are able to use methods like product() with the
    // matrix. In case of the builtin backend this is a noop, as only some
    // shared pointers are moved around.
    // The single-precision matrix will be moved to backend by the solver
    // constructor when it is done with analyzing the matrix.
    Ad->move_to_backend();

    // Initialize the solver with the system matrix.
    Solver solve(world, Af, prm);
    prof.toc("setup");
    //
    // Show the mini-report on the constructed solver:
    if (world.rank == 0) std::cout << solve << std::endl;

    // Local part of the solution vector:
    std::vector<double> x(chunk, 0.0);

    prof.tic("solve");
    // Reinterpret both the RHS and the solution vectors as block-valued:
    // Since the RHS in this case is filled with ones, the scaled RHS is equal to dia:
    auto f_ptr = reinterpret_cast<dvec_type*>(dia.data());
    auto x_ptr = reinterpret_cast<dvec_type*>(x.data());
    auto F = amgcl::make_iterator_range(f_ptr, f_ptr + chunk / B);
    auto X = amgcl::make_iterator_range(x_ptr, x_ptr + chunk / B);
    int iters;
    double error;
    std::tie(iters, error) = solve(*Ad, F, X);
    prof.toc("solve");

    if (world.rank == 0) std::cout
        << "Iterations: " << iters << std::endl
        << "Error:      " << error << std::endl
        << prof << std::endl;
}