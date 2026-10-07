#pragma once

#include <tracy/Tracy.hpp>

#ifdef RECOIL_DETAILED_TRACY_ZONING
	// line-unique variable: functions may also contain ZoneScoped / SCOPED_TIMER (___tracy_scoped_zone)
	#define RECOIL_TRACY_CAT_(a, b) a##b
	#define RECOIL_TRACY_CAT(a, b) RECOIL_TRACY_CAT_(a, b)
	#define RECOIL_DETAILED_TRACY_ZONE ZoneNamed(RECOIL_TRACY_CAT(___recoil_detailed_zone_, __LINE__), true)
#else
	#define RECOIL_DETAILED_TRACY_ZONE do {} while(0)
#endif