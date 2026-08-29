#include "core/logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>

namespace offcat {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_level(LogLevel level) {
    level_ = level;
}

LogLevel Logger::level() const {
    return level_;
}

static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

static void log_message(const std::string& prefix, const std::string& msg) {
    std::cerr << "[" << timestamp() << "] " << prefix << " " << msg << std::endl;
}

void Logger::quiet(const std::string& msg) {
    // Always shown
    std::cout << msg << std::endl;
}

void Logger::info(const std::string& msg) {
    if (level_ >= LogLevel::Normal) {
        std::cout << msg << std::endl;
    }
}

void Logger::verbose(const std::string& msg) {
    if (level_ >= LogLevel::Verbose) {
        log_message("[VERBOSE]", msg);
    }
}

void Logger::debug(const std::string& msg) {
    if (level_ >= LogLevel::Debug) {
        log_message("[DEBUG]", msg);
    }
}

void Logger::warning(const std::string& msg) {
    if (level_ >= LogLevel::Quiet) {
        log_message("[WARN]", msg);
    }
}

void Logger::error(const std::string& msg) {
    // Always shown
    log_message("[ERROR]", msg);
}

} // namespace offcat
