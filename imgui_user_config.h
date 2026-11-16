#pragma once
// Failed ImGui checks are logged instead of showing a blocking dialog (which could hide behind the overlay).
void ImAssertLog(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) do { if (!(_EXPR)) ImAssertLog(#_EXPR, __FILE__, __LINE__); } while (0)
