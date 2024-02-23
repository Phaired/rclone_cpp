#pragma once


#include "rclone_cpp/process.hpp"


#ifdef _WIN32
#define RCLONE_CPP_EXPORT __declspec(dllexport)
#else
#define RCLONE_CPP_EXPORT
#endif

namespace iridium::rclone
{

    RCLONE_CPP_EXPORT class process;
}
