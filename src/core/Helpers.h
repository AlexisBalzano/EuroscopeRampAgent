#pragma once

/**
* @brief Return a copy of the input string converted to uppercase.
* @param str The input string to convert.
* @return A new string that is the uppercase version of the input string.
*/
inline std::string ToUpper(std::string str)
{
	std::string result = str;
	std::transform(result.begin(), result.end(), result.begin(), ::toupper);
	return result;
}

/**
* @brief Wrap a C string returned by the Euroscope SDK, which may be null on an invalid object.
* @param str The C string to wrap, possibly nullptr.
* @return The string contents, or an empty string if the pointer is null.
*/
inline std::string SafeString(const char* str)
{
	return str != nullptr ? std::string(str) : std::string();
}

/**
* @brief Sort a list of stand names in a natural order, considering numeric prefixes and letter suffixes.
* @param standList The list of stand names to sort. The sorting is done in-place.
*/
inline void SortStandList(std::vector<std::string>& standList)
{
	std::sort(standList.begin(), standList.end(), [](const std::string& a, const std::string& b) {
		auto key = [](const std::string& s) {
			size_t i = 0, n = s.size();

			// Trim leading spaces
			while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i;

			// Leading number
			int num = 0;
			bool hasNum = false;
			while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) {
				hasNum = true;
				int digit = s[i] - '0';
				if (num > ((std::numeric_limits<int>::max)() - digit) / 10)
					num = (std::numeric_limits<int>::max)(); // clamp overflow
				else
					num = num * 10 + digit;
				++i;
			}

			// Immediate letter suffix (A, B, AB, ...)
			std::string letters;
			while (i < n && std::isalpha(static_cast<unsigned char>(s[i]))) {
				letters.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(s[i]))));
				++i;
			}

			// Remainder (case-insensitive)
			std::string tailUpper;
			tailUpper.reserve(n - i);
			for (; i < n; ++i) {
				unsigned char c = static_cast<unsigned char>(s[i]);
				tailUpper.push_back(static_cast<char>(std::toupper(c)));
			}

			// Bare names (no numeric prefix) go to the end
			return std::tuple<int, std::string, std::string, std::string>(
				hasNum ? num : (std::numeric_limits<int>::max)(), letters, tailUpper, s
			);
			};

		const auto [an, al, ar, as] = key(a);
		const auto [bn, bl, br, bs] = key(b);

		if (an != bn) return an < bn;

		// If numbers equal, empty suffix (e.g., "2") comes before "2A"
		if (al != bl) {
			if (al.empty() != bl.empty()) return al.empty();
			return al < bl;
		}

		// Fallback: remainder, then original
		if (ar != br) return ar < br;
		return as < bs;
		});
}