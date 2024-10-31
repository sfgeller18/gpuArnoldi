#ifndef SHIFT_HPP
#define SHIFT_HPP
    //TO-DO, write cuda kernel to simultaneously compute QR decomp of p = k - m shifted Hessenberg matrices.
    //Just setup blocks running Householder on each shifted matrix

    #include <vector>
    #include <algorithm>
    #include <complex>
    #include <numeric>
    #include <cstddef>
    #include "vector.hpp"
    #include "cuda_manager.hpp"
    #include "utils.hpp"

    #include <iostream>


    inline DeviceComplexType toDeviceComplex(const std::complex<double>& c) {
        return make_cuDoubleComplex(c.real(), c.imag());
    }

    // #define DEVICE_SHIFT

    using MatrixType = MatrixColMajor;

    inline int constructSMatrix(cusolverDnHandle_t solver_handle,
                            cublasHandle_t blas_handle,
                            const std::vector<DeviceComplexType*>& h_Aarray, 
                            const std::vector<DeviceComplexType*>& h_Tauarray,
                            const int m, //Basis Size
                            const int n, //Q column length (Original Space Size)
                            const int lda,
                            ComplexMatrix& S,
                            const int batch_count) {
        // Allocate device memory for Q matrix and result
        DeviceComplexType* d_Q;
        DeviceComplexType* d_S;
        DeviceComplexType* d_temp; // For intermediate results
        int* d_info;

        CHECK_CUDA(cudaMalloc(&d_Q, m * m * sizeof(DeviceComplexType)));
        CHECK_CUDA(cudaMalloc(&d_S, m * m * sizeof(DeviceComplexType)));
        CHECK_CUDA(cudaMalloc(&d_temp, m * m * sizeof(DeviceComplexType)));
        CHECK_CUDA(cudaMalloc(&d_info, sizeof(int)));

        // Buffer for Zungqr
        DeviceComplexType* d_work;
        int lwork;
        CHECK_CUSOLVER(cusolverDnZungqr_bufferSize(
            solver_handle, m, m, std::min(m,n),
            d_Q, lda,
            h_Tauarray[0],
            &lwork));
        CHECK_CUDA(cudaMalloc(&d_work, lwork * sizeof(DeviceComplexType)));

        // Constants for cublas
        const DeviceComplexType one = make_cuDoubleComplex(1.0, 0.0);
        const DeviceComplexType zero = make_cuDoubleComplex(0.0, 0.0);

        ComplexMatrix Q(m,n);
        for (int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
            // Copy the QR factored matrix to d_Q
            CHECK_CUDA(cudaMemcpy(d_Q, h_Aarray[batch_idx], 
                                m * n * sizeof(DeviceComplexType),
                                cudaMemcpyDeviceToDevice));
            
            // Construct Q matrix from the QR factorization
            CHECK_CUSOLVER(cusolverDnZungqr(
                solver_handle, m, m, std::min(m,n),
                d_Q, lda,
                h_Tauarray[batch_idx],
                d_work, lwork,
                d_info));

                // For first iteration, just copy Q to S
            if (batch_idx == 0) {
                CHECK_CUDA(cudaMemcpy(d_S, d_Q, m * m * sizeof(DeviceComplexType), cudaMemcpyDeviceToDevice));
            } else {CHECK_CUBLAS(cublasZgemm(blas_handle, CUBLAS_OP_N, CUBLAS_OP_N, m, m, m, &one, d_S, m, d_Q, m, &zero, d_S, m));}
        }

        // Copy final result back to host
        CHECK_CUDA(cudaMemcpy(S.data(), d_S,
                            m * m * sizeof(DeviceComplexType),
                            cudaMemcpyDeviceToHost));

        print(S);
        
        // Clean up
        CHECK_CUDA(cudaFree(d_Q));
        CHECK_CUDA(cudaFree(d_S));
        CHECK_CUDA(cudaFree(d_temp));
        CHECK_CUDA(cudaFree(d_info));
        CHECK_CUDA(cudaFree(d_work));

        return 0;
    }

    inline int computeShift(ComplexMatrix& S,
                    const ComplexMatrix& complex_M, 
                    const ComplexVector& eigenvalues,
                    const size_t& N,
                    const size_t& m,
                    cublasHandle_t& handle,
                    cusolverDnHandle_t& solver_handle) {

        assert(m < N);
        assert(isHessenberg(complex_M));

        const size_t N_squared = N * N;
        const size_t batch_count = N - m;
        int info;

        DeviceComplexType* d_matrices = cudaMallocChecked<DeviceComplexType>(batch_count * N_squared * sizeof(DeviceComplexType));
        DeviceComplexType* d_tau = cudaMallocChecked<DeviceComplexType>(batch_count * N * sizeof(DeviceComplexType));
        DeviceComplexType** d_Aarray = cudaMallocChecked<DeviceComplexType*>(batch_count * sizeof(DeviceComplexType*));
        DeviceComplexType** d_Tauarray = cudaMallocChecked<DeviceComplexType*>(batch_count * sizeof(DeviceComplexType*));
        std::vector<size_t> indices(batch_count);

        //Host Array of Device Ptrs for cublas
        std::vector<DeviceComplexType*> h_Aarray(batch_count);
        std::vector<DeviceComplexType*> h_Tauarray(batch_count);

        std::iota(indices.begin(), indices.end(), 0);
        std::sort(indices.begin(), indices.end(),
                [&eigenvalues](size_t i1, size_t i2) { return std::norm(eigenvalues[i1]) < std::norm(eigenvalues[i2]); });

        ComplexVector smallest_eigenvalues(batch_count);
        for (size_t i = 0; i < batch_count; ++i) {
            smallest_eigenvalues[i] = eigenvalues[indices[i]];
        }

        // Prepare matrices on host
        std::vector<DeviceComplexType> h_matrices(batch_count * N_squared);
        for (int k = 0; k < batch_count; k++) {
            for (int j = 0; j < N; j++) {
                for (int i = 0; i < N; i++) {
                    size_t idx = k * N_squared + j * N + i;
                    if (i == j) {
                        h_matrices[idx] = toDeviceComplex(complex_M(j,i) - smallest_eigenvalues[k]);
                    } else {
                        h_matrices[idx] = toDeviceComplex(complex_M(j,i));
                    }
                }
            }
        }

        // Copy matrices to device
        CHECK_CUDA(cudaMemcpy(d_matrices, h_matrices.data(), 
                            batch_count * N_squared * sizeof(DeviceComplexType), 
                            cudaMemcpyHostToDevice));

        // Set up host arrays of device pointers for cublas
        for (int i = 0; i < batch_count; i++) {
            h_Aarray[i] = d_matrices + i * N_squared;
            h_Tauarray[i] = d_tau + i * N;
        }

        // Copy pointer arrays to device
        cudaMemcpyChecked(d_Aarray, h_Aarray.data(), batch_count * sizeof(DeviceComplexType*), cudaMemcpyHostToDevice);
        cudaMemcpyChecked(d_Tauarray, h_Tauarray.data(), batch_count * sizeof(DeviceComplexType*), cudaMemcpyHostToDevice);

        CHECK_CUBLAS(cublasZgeqrfBatched(handle, N, N, d_Aarray, N, d_Tauarray, &info, batch_count));

        // Pass the device array pointer and host tau array
        constructSMatrix(solver_handle, handle, h_Aarray, h_Tauarray, N, N, N, S, batch_count);
        
        assert(isOrthonormal<ComplexMatrix>(S));

        cudaFreeChecked(d_matrices);
        cudaFreeChecked(d_tau);
        cudaFreeChecked(d_Aarray);
        cudaFreeChecked(d_Tauarray);

        return 0;
    }

    // int main() {
    //     const size_t N = 5;
    //     const size_t k = 2;
    //     ComplexVector evals(N);
    //     ComplexMatrix evecs(N,N);
    //     ComplexMatrix H = generateRandomHessenbergMatrix<ComplexMatrix>(N);
    //     ComplexMatrix V = gramSchmidtOrthonormal(N);

    //     eigsolver(H, evals, evecs);
    //     reduceEvals(H, V, N, k);
    //     print(V);
    // }

#endif
