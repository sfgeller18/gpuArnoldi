# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.30

# compile CUDA with /opt/cuda/bin/nvcc
# compile CXX with /usr/bin/clang++
CUDA_DEFINES = -DPRECISION_DOUBLE -DUSE_EIGEN

CUDA_INCLUDES = --options-file CMakeFiles/cuda_demo.dir/includes_CUDA.rsp

CUDA_FLAGS =  -diag-suppress 20012 -std=c++20 "--generate-code=arch=compute_75,code=[compute_75,sm_75]"

CXX_DEFINES = -DPRECISION_DOUBLE -DUSE_EIGEN

CXX_INCLUDES = -I/opt/cuda/include -I/home/sfg18/projects/neurocpp/include -I/usr/include/eigen3

CXX_FLAGS = -std=gnu++23

