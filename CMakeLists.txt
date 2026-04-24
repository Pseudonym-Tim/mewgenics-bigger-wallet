cmake_minimum_required(VERSION 3.24)
project(BiggerWallet LANGUAGES C)

add_library(BiggerWallet SHARED
  src/BiggerWallet.c
)

target_compile_features(BiggerWallet PRIVATE c_std_11)

target_compile_definitions(BiggerWallet PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

set_target_properties(BiggerWallet PROPERTIES
  OUTPUT_NAME "BiggerWallet"
)
