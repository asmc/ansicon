/*
  version.h - Version defines.
*/

#define PVERS	L"1.70"         // wide string
#define PVERSA	 "1.70"         // ANSI string (windres 2.16.91 didn't like L)
#define PVERE	L"170"          // wide environment string
#define PVEREA	 "170"          // ANSI environment string
#define PVERB	1,7,0,0 	// binary (resource)

#ifdef _WIN64
# define BITS L"64"
# define BITSA "64"
#else
# define BITS L"32"
# define BITSA "32"
#endif

#define ANSIDLL L"ANSI" BITS L".dll"
