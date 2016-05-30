#pragma once

#include <string>

namespace ioremap { namespace nulla {

static std::string metadata_key(const std::string &base) {
	static const std::string prefix = "nulla.meta";

	char tmp[prefix.size() + 1 + base.size() + 1];
	int sz = snprintf(tmp, sizeof(tmp), "%s.%.*s", prefix.c_str(), (int)base.size(), base.c_str());
	tmp[prefix.size()] = '\0';

	return std::string(tmp, sz);
}

}} // namespace ioremap::nulla
