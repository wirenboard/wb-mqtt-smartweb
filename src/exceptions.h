#pragma once

#include <stdexcept>
#include <string>

class TDriverError: public std::runtime_error
{
public:
    explicit TDriverError(const std::string& what): std::runtime_error(what)
    {}
};

class TUnsupportedError: public TDriverError
{
public:
    explicit TUnsupportedError(const std::string& what): TDriverError(what)
    {}
};

class TFrameError: public TDriverError
{
public:
    explicit TFrameError(const std::string& what): TDriverError(what)
    {}
};
