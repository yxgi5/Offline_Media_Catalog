#pragma once

#include <string>
#include <functional>
#include <cstdarg>

namespace offcat {

enum class LogLevel {
    Quiet   = 0,
    Normal  = 1,
    Verbose = 2,
    Debug   = 3
};

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level);
    LogLevel level() const;

    void quiet(const std::string& msg);
    void info(const std::string& msg);
    void verbose(const std::string& msg);
    void debug(const std::string& msg);
    void warning(const std::string& msg);
    void error(const std::string& msg);

private:
    Logger() = default;
    LogLevel level_ = LogLevel::Normal;
};

// Convenience macros
#define LOG_QUIET(msg)   offcat::Logger::instance().quiet(msg)
#define LOG_INFO(msg)    offcat::Logger::instance().info(msg)
#define LOG_VERBOSE(msg) offcat::Logger::instance().verbose(msg)
#define LOG_DEBUG(msg)   offcat::Logger::instance().debug(msg)
#define LOG_WARN(msg)    offcat::Logger::instance().warning(msg)
#define LOG_ERROR(msg)   offcat::Logger::instance().error(msg)

} // namespace offcat
