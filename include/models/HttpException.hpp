#pragma once
#include <stdexcept>
#include <string>

struct HttpException : std::exception {
    int         status_code;
    std::string message;

    HttpException(int code, std::string msg)
        : status_code(code), message(std::move(msg)) {}

    const char* what() const noexcept override { return message.c_str(); }
};
