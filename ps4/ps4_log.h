#pragma once

#include <format>
#include <string>

void ps4_log_init();
void ps4_log_line(const char *level, const std::string &text);

#define PS4_LOG(level, ...) ps4_log_line(level, std::format(__VA_ARGS__))
#define LOG_INFO(...) PS4_LOG("I", __VA_ARGS__)
#define LOG_WARN(...) PS4_LOG("W", __VA_ARGS__)
#define LOG_ERROR(...) PS4_LOG("E", __VA_ARGS__)
