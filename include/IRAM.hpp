#ifndef IRAM_HPP
#define IRAM_HPP

#include "arnoldi.hpp"
#include "shift.hpp"

// #define DBG_INTERNALS
#ifdef DBG_INTERNALS
template <typename M, typename DS, size_t N, size_t A, size_t B, size_t C>
void IRAM_dbg_check(DS* d_evecs, DS* d_h, const M& Q, const M& H_tilde) {
    constexpr size_t ALLOC_SIZE = BasisTraits<M>::ALLOC_SIZE;
    M initial_evecs = M::Zero(N, B + 1);
    M initial_h = M::Zero(B + 1, B);
    cudaMemcpyChecked(initial_evecs.data(), d_evecs, N * (B+1) * ALLOC_SIZE, cudaMemcpyDeviceToHost);
    cudaMemcpyChecked(initial_h.data(), d_h, (B + 1) * B * ALLOC_SIZE, cudaMemcpyDeviceToHost);
    // std::cout << "Initial Evecs:\n" << initial_evecs << std::endl;
    assert(initial_h.isApprox(H_tilde));
    assert(initial_evecs.block(0,0,N,C).isApprox(Q.block(0,0,N,C)));
    assert(initial_evecs.block(0,C,N,B+1-C).isApprox(Q.block(0,C,N,B+1-C)));
}
#endif

template <typename M, size_t N, size_t A, size_t B, size_t C> //A is max iters, B is basis size, C is restart size
ComplexEigenPairs IRAM(const M& M_, cublasHandle_t& handle, cusolverDnHandle_t& solver_handle, const HostPrecision& tol = default_tol) {
    using S = typename BasisTraits<M>::S;
    using DS = typename BasisTraits<M>::DS;
    using V = typename BasisTraits<M>::V;
    using OM = typename BasisTraits<M>::OM;
    constexpr size_t ALLOC_SIZE = BasisTraits<M>::ALLOC_SIZE;
    const HostPrecision matnorm = M_.norm();

    OM Q(N, B + 1);
    OM H_tilde(B + 1, B);

    assert(B < N && "max_iters must be leq than leading dimension of M");
    Vector norms(B);
    V v0 = randVecGen<V>(N);



    size_t m = 1;
    const size_t ROWS = DYNAMIC_ROW_ALLOC(N);

    // CUDA Allocations
    DS* d_evecs = cudaMallocChecked<DS>((B + 1) * N * ALLOC_SIZE);
    DS* d_proj = cudaMallocChecked<DS>((B + 1) * ALLOC_SIZE);
    DS* d_y = cudaMallocChecked<DS>(N * ALLOC_SIZE);
    DS* d_M = cudaMallocChecked<DS>(ROWS * N * ALLOC_SIZE);
    DS* d_result = cudaMallocChecked<DS>(N * ALLOC_SIZE);
    DS* d_h = cudaMallocChecked<DS>((B + 1) * B * ALLOC_SIZE);

    // Initial setup
    cudaMemcpyChecked(d_y, v0.data(), N * ALLOC_SIZE, cudaMemcpyHostToDevice);
    cudaMemcpyChecked(d_evecs, v0.data(), N * ALLOC_SIZE, cudaMemcpyHostToDevice);

    ComplexMatrix Q_block(N, B);
    ComplexMatrix H_square(B, B);

    const size_t num_loops = std::ceil(A / B);
    std::cout << "Entering Arnoldi Iteration" << std::endl;
    for (int i = 0; i < num_loops; i++) {
        auto start_iter = std::chrono::high_resolution_clock::now();
        if (i == 0) {KrylovIterInternal<M, DS, N, N, B>(M_, d_M, d_y, d_result, d_evecs, d_h, norms, ROWS, handle, matnorm);}
        else {        
            cudaMemcpyChecked(d_evecs, Q.data(), N * C * ALLOC_SIZE, cudaMemcpyHostToDevice); //Ideally looking to make the shifting be on GPU to avoid memcpy, but not end of world
            cudaMemset(d_evecs + N * C, 0, N * (B + 1 - C) * ALLOC_SIZE);
            cudaMemcpyChecked(d_h, H_tilde.data(), (B+1) * C * ALLOC_SIZE, cudaMemcpyHostToDevice);
            cudaMemset(d_h + (B+1) * C, 0, (B+1) * (B - C) * ALLOC_SIZE); //Since is Hessenberg, we just need to set subsequent cols to zero
            cudaMemcpyChecked(d_y, d_evecs, N * ALLOC_SIZE, cudaMemcpyDeviceToDevice);
                        
            #ifdef DBG_INTERNALS
            IRAM_dbg_check<M, DS, N, A, B, C>(d_evecs, d_h, Q, H_tilde);
            #endif

            cudaMemcpyChecked(d_y, v0.data(), N * ALLOC_SIZE, cudaMemcpyHostToDevice);
            KrylovIterInternal<M, DS, N, N, B, C - 1>(M_, d_M, d_y, d_result, d_evecs, d_h, norms, ROWS, handle, matnorm);
        }
        cudaMemcpyChecked(Q.data(), d_evecs, N * (B + 1) * ALLOC_SIZE, cudaMemcpyDeviceToHost);
        cudaMemcpyChecked(H_tilde.data(), d_h, (B + 1) * B * ALLOC_SIZE, cudaMemcpyDeviceToHost);
        for (int j = 0; j < B; ++j) { H_tilde(j + 1, j) = norms[j]; } // Insert norms back into Hessenberg diagonal

        assert(isOrthonormal<OM>(Q.block(0,0,N,10)));
        // assert(isHessenberg<OM>(H_tilde));

        auto end_iter = std::chrono::high_resolution_clock::now();
        std::cout << "Arnoldi Iteration " << i << ", Performed in :"
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end_iter - start_iter).count()
                  << " ms" << std::endl;
        auto start_reduce = std::chrono::high_resolution_clock::now();
        reduceArnoldiPairInternal<M, N, B>(Q, H_tilde, C, handle, solver_handle, H_square, Q_block);
        auto end_reduce = std::chrono::high_resolution_clock::now();
        std::cout << "Arnoldi Reduction " << i << ", Performed in :"
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end_reduce - start_reduce).count()
                  << " ms" << std::endl;

        assert(isOrthonormal<OM>(Q.leftCols(C)));
        assert(isHessenberg<OM>(H_tilde.block(0,0,C, C)));
        }

    // Free device memory
    cudaFree(d_evecs);
    cudaFree(d_proj);
    cudaFree(d_y);
    cudaFree(d_M);
    cudaFree(d_result);
    cudaFree(d_h);

    ComplexEigenPairs ritzPairs{};
    hessEigSolver<ComplexMatrix>(H_tilde.block(0,0,C, C), ritzPairs, C);
    // std::cout << ritzPairs.vectors.cols() << " " << ritzPairs.vectors.rows() << std::endl;
    // std::cout << Q.leftCols(C) << std::endl;
    return {ritzPairs.values, Q.leftCols(C) * ritzPairs.vectors, C};


}


#endif //IRAM_HPP