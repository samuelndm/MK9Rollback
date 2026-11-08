#pragma once
#include <string>

// Returns the uppercase hex SHA-256 of a file, or an empty string on failure.
std::string Sha256File(const wchar_t* path);
