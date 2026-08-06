#pragma once

/// Concatenates two tokens after they've been macro-expanded (e.g. __LINE__),
/// for mangling a unique variable name at each call site of a scope macro.
#define CS_DETAIL_CONCAT_IMPL(a, b) a##b
#define CS_DETAIL_CONCAT(a, b) CS_DETAIL_CONCAT_IMPL(a, b)
