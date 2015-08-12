/*
  ANSICON.c - ANSI escape sequence console driver.

  Jason Hood, 21 to 23 October, 2005.

  Original injection code was derived from Console Manager by Sergey Oblomov
  (hoopoepg).  Use of FlushInstructionCache came from www.catch22.net.
  Additional information came from "Process-wide API spying - an ultimate hack",
  Anton Bassov's article in "The Code Project" (use of OpenThread).

  v1.01, 11 & 12 March, 2006:
    -m option to set "monochrome" (grey on black);
    restore original color on exit.

  v1.10, 22 February, 2009:
    ignore Ctrl+C/Ctrl+Break.

  v1.13, 21 & 27 March, 2009:
    alternate injection method, to work with DEP;
    use Unicode.

  v1.20, 17 to 21 June, 2009:
    use a combination of the two injection methods;
    test if ANSICON is already installed;
    added -e (and -E) option to echo the command line (without newline);
    added -t (and -T) option to type (display) files (with file name).

  v1.21, 23 September, 2009:
    added -i (and -u) to add (remove) ANSICON to AutoRun.

  v1.24, 6 & 7 January, 2010:
    no arguments to -t, or using "-" for the name, will read from stdin;
    fix -t and -e when ANSICON was already loaded.

  v1.25, 22 July, 2010:
    added -IU for HKLM.

  v1.30, 3 August to 7 September, 2010:
    x64 support.

  v1.31, 13 & 15 November, 2010:
    use LLW to fix potential Unicode path problems;
    VC compatibility (2008 Express for 32-bit, PSDK 2003 R2 for 64-bit);
    explicitly use wide characters (stick with TCHAR, but not <tchar.h>).

  v1.32, 4 to 22 December, 2010:
    make -p more robust;
    inject into GUI processes;
    -i implies -p.

  v1.50, 7 to 14 December, 2011:
    -u does not imply -p;
    add the PID to the debugging output;
    use ANSICON_VER to test if already installed;
    always place first in AutoRun;
    logging is always available, controlled by ANSICON_LOG environment variable;
    only restore the original color after program/echo/type;
    return program's exit code.

  7 January, 2012:
    fixed installing into a piped CMD.EXE;
    added a log message indicating all imports have been processed.

  v1.52, 10 April, 2012:
    fixed running "cmd" if "ComSpec" is not defined;
    pass process & thread identifiers on the command line (for x86->x64).

  v1.60, 22 & 24 November, 2012:
    set the code page to convert strings correctly;
    expand wildcards for -t;
    write the date if appending to the log.

  v1.62, 18 July, 2013:
    write the bits to the log;
    test if creating the registry key fails (HKLM requires admin privileges).

  v1.63, 25 July, 2013:
    don't write the reset sequence if output is redirected.

  v1.70, 31 January to 7 February, 2014:
    restore the original (current, not default) attributes if using ansicon.exe
     when it's already installed;
    use ANSICON_DEF if defined and -m not given;
    -e and -t will not output anything if the DLL could not load;
    use Unicode output (_O_U16TEXT, for compilers/systems that support it);
    log: 64-bit addresses get an underscore between the 8-digit groups;
	 add error codes to some message.
*/

#define PDATE L"26 February, 2014"

#include "ansicon.h"
#include "version.h"
#include <tlhelp32.h>
#include <ctype.h>
#include <fcntl.h>
#include <io.h>
#include <locale.h>

#ifndef _O_U16TEXT
#define _O_U16TEXT 0x20000
#endif

#ifdef __MINGW32__
int _CRT_glob = 0;
#endif


#define CMDKEY	L"Software\\Microsoft\\Command Processor"
#define AUTORUN L"AutoRun"


void   help( void );

void   display( LPCTSTR, BOOL );
void   print_error( LPCTSTR );
LPTSTR skip_spaces( LPTSTR );
void   get_arg( LPTSTR, LPTSTR*, LPTSTR* );
void   get_file( LPTSTR, LPTSTR*, LPTSTR* );

void   process_autorun( TCHAR );

BOOL   find_proc_id( HANDLE snap, DWORD id, LPPROCESSENTRY32 ppe );
BOOL   GetParentProcessInfo( LPPROCESS_INFORMATION ppi, LPTSTR );


static HANDLE hConOut;
static WORD   wAttr;

void get_original_attr( void )
{
  CONSOLE_SCREEN_BUFFER_INFO csbi;

  hConOut = CreateFile( L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
				    FILE_SHARE_READ | FILE_SHARE_WRITE,
				    NULL, OPEN_EXISTING, 0, 0 );
  GetConsoleScreenBufferInfo( hConOut, &csbi );
  wAttr = csbi.wAttributes;
}


void set_original_attr( void )
{
  SetConsoleTextAttribute( hConOut, wAttr );
  CloseHandle( hConOut );
}


// The fputws function in MSVCRT.DLL (Windows 7 x64) is broken for Unicode
// output (it just writes the first character).  VC6 & 7 don't support Unicode
// output at all (just converting to ANSI) and even when it is supported, it
// just writes single characters (as does _putws & fwprintf).  So what the
// heck, DIY.
int my_fputws( const wchar_t* s, FILE* f )
{
  if (_isatty( _fileno( f ) ))
  {
    DWORD written;
    WriteConsole( hConOut, s, (DWORD)wcslen( s ), &written, NULL );
  }
  else
  {
    fputws( s, f );
  }
  return 0;
}

#define fputws	    my_fputws
#define _putws( s ) my_fputws( s L"\n", stdout )


#if defined(_WIN64)
LPTSTR DllNameType;
#endif

// Find the name of the DLL and inject it.
BOOL Inject( LPPROCESS_INFORMATION ppi, BOOL* gui, LPCTSTR app )
{
  DWORD len;
  int	type;
  PBYTE base;

#ifdef _WIN64
  if (app != NULL)
#endif
  DEBUGSTR( 1, L"%s (%lu)", app, ppi->dwProcessId );
  type = ProcessType( ppi, &base, gui );
  if (type <= 0)
  {
    if (type == 0)
      fwprintf( stderr, L"ANSICON: %s: unsupported process.\n", app );
    return FALSE;
  }

  len = (DWORD)(prog - prog_path);
  memcpy( DllName, prog_path, TSIZE(len) );
#ifdef _WIN64
  wsprintf( DllName + len, L"ANSI%d.dll", (type == 48) ? 64 : type );
  DllNameType = DllName + len + 4;
  set_ansi_dll();
  if (type == 64)
    InjectDLL( ppi, base );
  else if (type == 32)
    InjectDLL32( ppi, base );
  else // (type == 48)
    InjectDLL64( ppi );
#else
  wcscpy( DllName + len, L"ANSI32.dll" );
  set_ansi_dll();
  InjectDLL( ppi, base );
#endif

  return TRUE;
}


// Use CreateRemoteThread to load our DLL in the target process.
void RemoteLoad( LPPROCESS_INFORMATION ppi, LPCTSTR app )
{
  HANDLE hSnap;
  MODULEENTRY32 me;
  PBYTE  LLW;
  BOOL	 fOk;
  DWORD  len;
  LPVOID mem;
  HANDLE thread;
  DWORD  ticks;
#ifdef _WIN64
  BOOL	 WOW64;
  int	 type;
#endif

  DEBUGSTR( 1, L"%s (%lu)", app, ppi->dwProcessId );

  // Find the base address of kernel32.dll.
  ticks = GetTickCount();
  while ((hSnap = CreateToolhelp32Snapshot(
		   TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, ppi->dwProcessId ))
		== INVALID_HANDLE_VALUE)
  {
    DWORD err = GetLastError();
#ifndef _WIN64
    if (err == ERROR_PARTIAL_COPY)
    {
      DEBUGSTR( 1, L"  Ignoring 64-bit process (use x64\\ansicon)" );
      fputws( L"ANSICON: parent is 64-bit (use x64\\ansicon).\n", stderr );
      return;
    }
#endif
    // I really don't think this would happen, but if it does, give up after
    // two seconds to avoid a potentially infinite loop.
    if (err == ERROR_BAD_LENGTH && GetTickCount() - ticks < 2000)
    {
      Sleep( 0 );
      continue;
    }
    DEBUGSTR( 1, L"  Unable to create snapshot (%lu)", err );
  no_go:
    fputws( L"ANSICON: unable to inject into parent.\n", stderr );
    return;
  }
  LLW = NULL;
  me.dwSize = sizeof(MODULEENTRY32);
  for (fOk = Module32First( hSnap, &me ); fOk; fOk = Module32Next( hSnap, &me ))
  {
    if (_wcsicmp( me.szModule, L"kernel32.dll" ) == 0)
    {
      LLW = me.modBaseAddr;
      break;
    }
  }
  CloseHandle( hSnap );
  if (LLW == NULL)
  {
    DEBUGSTR( 1, L"  Unable to locate kernel32.dll (%lu)", GetLastError() );
    goto no_go;
  }

  len = (DWORD)(prog - prog_path);
  memcpy( DllName, prog_path, TSIZE(len) );
#ifdef _WIN64
  type = (IsWow64Process( ppi->hProcess, &WOW64 ) && WOW64) ? 32 : 64;
  wsprintf( DllName + len, L"ANSI%d.dll", type );
  LLW += GetProcRVA( L"kernel32.dll", "LoadLibraryW", type );
#else
  wcscpy( DllName + len, L"ANSI32.dll" );
  LLW += GetProcRVA( L"kernel32.dll", "LoadLibraryW" );
#endif
  if (LLW == me.modBaseAddr)
    goto no_go;

  mem = VirtualAllocEx( ppi->hProcess, NULL, len, MEM_COMMIT, PAGE_READWRITE );
  if (mem == NULL)
  {
    DEBUGSTR( 1, L"  Unable to allocate virtual memory (%lu)", GetLastError() );
    goto no_go;
  }
  WriteProcMem( mem, DllName, TSIZE(len + 11) );
  thread = CreateRemoteThread( ppi->hProcess, NULL, 4096,
			       (LPTHREAD_START_ROUTINE)LLW, mem, 0, NULL );
  WaitForSingleObject( thread, INFINITE );
  CloseHandle( thread );
  VirtualFreeEx( ppi->hProcess, mem, 0, MEM_RELEASE );
}


DWORD CtrlHandler( DWORD event )
{
  return (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT);
}


int main( void )
{
  STARTUPINFO si;
  PROCESS_INFORMATION pi;
  LPTSTR  argv, arg, cmd;
  TCHAR   buf[4];
  BOOL	  shell, run, gui;
  HMODULE ansi;
  DWORD   len;
  int	  rc = 0;

  // Convert wide strings using the current code page.
  sprintf( (LPSTR)buf, ".%u", GetConsoleOutputCP() );
  setlocale( LC_CTYPE, (LPSTR)buf );

  // Switch console output to Unicode.
  if (_isatty( 1 ))
    _setmode( 1, _O_U16TEXT);
  if (_isatty( 2 ))
    _setmode( 2, _O_U16TEXT);

  // Create a console handle and store the current attributes.
  get_original_attr();

  argv = GetCommandLine();
  len = (DWORD)wcslen( argv ) + 1;
  if (len < MAX_PATH)
    len = MAX_PATH;
  arg = malloc( TSIZE(len) );
  get_arg( arg, &argv, &cmd );	// skip the program name
  get_arg( arg, &argv, &cmd );

  if (*arg)
  {
    if (wcscmp( arg, L"/?" ) == 0 ||
	wcscmp( arg, L"--help" ) == 0)
    {
      help();
      return rc;
    }
    if (wcscmp( arg, L"--version" ) == 0)
    {
      _putws( L"ANSICON (" BITS L"-bit) version " PVERS L" (" PDATE L")." );
      return rc;
    }
  }

  prog = get_program_name( NULL );
  *buf = '\0';
  GetEnvironmentVariable( L"ANSICON_LOG", buf, lenof(buf) );
  log_level = _wtoi( buf );

#ifdef _WIN64
  if (*arg == '-' && arg[1] == 'P')
  {
    pi.dwProcessId = _wtoi( arg + 2 );
    pi.hProcess = OpenProcess( PROCESS_ALL_ACCESS, FALSE, pi.dwProcessId );
    if (pi.hProcess == NULL)
    {
      DEBUGSTR( 1, L"  Unable to open process %lu (%lu)",
		   pi.dwProcessId, GetLastError() );
    }
    else
    {
      Inject( &pi, &gui, NULL );
      CloseHandle( pi.hProcess );
    }
    return 0;
  }
#endif

  if (log_level)
    DEBUGSTR( 1, NULL );	// start a new session

  shell = run = TRUE;

  while (*arg == '-')
  {
    switch (arg[1])
    {
      case 'l':
	SetEnvironmentVariable( L"ANSICON_LOG", arg + 2 );
	log_level = _wtoi( arg + 2 );
	DEBUGSTR( 1, NULL );		// create a session
	break;

      case 'i':
      case 'I':
      case 'u':
      case 'U':
	shell = FALSE;
	process_autorun( arg[1] );
	if (arg[1] == 'u' || arg[1] == 'U')
	  break;
	// else fall through

      case 'p':
	shell = FALSE;
	if (GetParentProcessInfo( &pi, arg ))
	{
	  pi.hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pi.dwProcessId);
	  RemoteLoad( &pi, arg );
	  CloseHandle( pi.hProcess );
	}
	else
	{
	  fputws( L"ANSICON: could not obtain the parent process.\n", stderr );
	  rc = 1;
	}
	break;

      case 'm':
      {
	int a = wcstol( arg + 2, NULL, 16 );
	if (a == 0)
	  a = (arg[2] == '-') ? -7 : 7;
	if (a < 0)
	{
	  SetEnvironmentVariable( L"ANSICON_REVERSE", L"1" );
	  a = -a;
	  a = ((a >> 4) & 15) | ((a & 15) << 4);
	}
	SetConsoleTextAttribute( hConOut, (WORD)a );
	SetEnvironmentVariable( L"ANSICON_DEF", NULL );
	break;
      }

      case 'e':
      case 'E':
      case 't':
      case 'T':
	run = FALSE;
	++arg;
	goto arg_out;
    }
    get_arg( arg, &argv, &cmd );
  }
arg_out:
  if (run && *cmd == '\0')
  {
    if (!shell)
      run = FALSE;
    else if (!_isatty( 0 ))
    {
      *arg = 't';
      run = FALSE;
    }
  }

  // Ensure the default attributes are the current attributes.
  if (GetEnvironmentVariable( L"ANSICON_DEF", buf, lenof(buf) ) != 0)
  {
    int a = wcstol( buf, NULL, 16 );
    if (a < 0)
      a = ((-a >> 4) & 15) | ((-a & 15) << 4);
    SetConsoleTextAttribute( hConOut, (WORD)a );
  }

  if (run)
  {
    if (*cmd == '\0')
    {
      if (GetEnvironmentVariable( L"ComSpec", arg, MAX_PATH ))
	cmd = arg;
      else
      {
	// CreateProcessW writes to the string, so can't simply point to "cmd".
	static TCHAR cmdstr[] = L"cmd";
	cmd = cmdstr;
      }
    }

    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(si);
    if (CreateProcess( NULL, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED,
		       NULL, NULL, &si, &pi ))
    {
      Inject( &pi, &gui, arg );
      ResumeThread( pi.hThread );
      if (!gui)
      {
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE)CtrlHandler, TRUE );
	WaitForSingleObject( pi.hProcess, INFINITE );
	GetExitCodeProcess( pi.hProcess, (LPDWORD)(LPVOID)&rc );
      }
      CloseHandle( pi.hProcess );
      CloseHandle( pi.hThread );
    }
    else
    {
      print_error( arg );
      rc = 1;
    }
  }
  else if (*arg)
  {
    ansi = LoadLibrary( ANSIDLL );
    if (ansi == NULL)
    {
      print_error( ANSIDLL );
      rc = 1;
    }
    else if (*arg == 'e' || *arg == 'E')
    {
      cmd += 2;
      if (*cmd == ' ' || *cmd == '\t')
	++cmd;
      fputws( cmd, stdout );
      if (*arg == 'e')
	putwchar( '\n' );
    }
    else // (*arg == 't' || *arg == 'T')
    {
      BOOL title = (*arg == 'T');
      get_file( arg, &argv, &cmd );
      if (*arg == '\0')
	wcscpy( arg, L"-" );
      do
      {
	if (title)
	{
	  wprintf( L"==> %s <==\n", arg );
	  display( arg, title );
	  putwchar( '\n' );
	}
	else
	  display( arg, title );
	get_file( arg, &argv, &cmd );
      } while (*arg);
    }
    FreeLibrary( ansi );
  }

  set_original_attr();

  return rc;
}


// Display a file.
void display( LPCTSTR name, BOOL title )
{
  HANDLE in, out;
  BOOL	 pipe;
  char	 buf[8192];
  DWORD  len;

  if (*name == '-' && name[1] == '\0')
  {
    pipe = TRUE;
    in = GetStdHandle( STD_INPUT_HANDLE );
  }
  else
  {
    pipe = FALSE;
    in = CreateFile( name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		     NULL, OPEN_EXISTING, 0, NULL );
    if (in == INVALID_HANDLE_VALUE)
    {
      print_error( name );
      return;
    }
  }
  if (title)
  {
    putwchar( '\n' );
    // Need to flush, otherwise it's written *after* STD_OUTPUT_HANDLE should
    // it be redirected.
    fflush( stdout );
  }
  out = GetStdHandle( STD_OUTPUT_HANDLE );
  for (;;)
  {
    if (!ReadFile( in, buf, sizeof(buf), &len, NULL ))
    {
      if (GetLastError() != ERROR_BROKEN_PIPE)
	print_error( name );
      break;
    }
    if (len == 0)
      break;
    WriteFile( out, buf, len, &len, NULL );
  }
  if (!pipe)
    CloseHandle( in );
}


void print_error( LPCTSTR name )
{
  LPTSTR errmsg = NULL;
  DWORD  err = GetLastError();

  fputws( L"ANSICON: ", stderr );
  if (err == ERROR_BAD_EXE_FORMAT)
  {
    // This error requires an argument, which is name.
    FormatMessage( FORMAT_MESSAGE_FROM_SYSTEM |
		   FORMAT_MESSAGE_ALLOCATE_BUFFER |
		   FORMAT_MESSAGE_ARGUMENT_ARRAY,
		   NULL, err, 0, (LPTSTR)(LPVOID)&errmsg, 0, (va_list*)&name );
    fputws( errmsg, stderr );
  }
  else
  {
    if (FormatMessage( FORMAT_MESSAGE_FROM_SYSTEM |
		       FORMAT_MESSAGE_ALLOCATE_BUFFER |
		       FORMAT_MESSAGE_IGNORE_INSERTS,
		       NULL, err, 0, (LPTSTR)(LPVOID)&errmsg, 0, NULL ))
      fwprintf( stderr, L"%s: %s", name, errmsg );
    else
      fwprintf( stderr, L"%s: Error %lu.\n", name, err );
  }
  LocalFree( errmsg );
}


// Add or remove ANSICON to AutoRun.
void process_autorun( TCHAR cmd )
{
  HKEY	 cmdkey;
  TCHAR  ansicon[MAX_PATH+80];
  TCHAR  logstr[80];
  LPTSTR autorun, ansirun;
  DWORD  len, type, exist;
  BOOL	 inst;

  if (log_level)
    _snwprintf( logstr, lenof(logstr), L"set ANSICON_LOG=%d&", log_level );
  else
    *logstr = '\0';
  len = TSIZE(_snwprintf( ansicon, lenof(ansicon),
		L"(if %%ANSICON_VER%%==^%%ANSICON_VER^%% %s\"%s\" -p)",
		logstr, prog_path ) + 1);

  inst = (towlower( cmd ) == 'i');
  if (RegCreateKeyEx( (iswlower(cmd)) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE,
		      CMDKEY, 0, NULL,
		      REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL,
		      &cmdkey, &exist ) != ERROR_SUCCESS)
  {
    fputws( L"ANSICON: could not update AutoRun", stderr );
    if (iswupper( cmd ))
      fwprintf( stderr, L" (perhaps use -%c, or run as admin)", towlower(cmd) );
    fputws( L".\n", stderr );
  }
  exist = 0;
  RegQueryValueEx( cmdkey, AUTORUN, NULL, NULL, NULL, &exist );
  if (exist == 0)
  {
    if (inst)
      RegSetValueEx( cmdkey, AUTORUN, 0, REG_SZ, (PBYTE)ansicon, len );
  }
  else
  {
    // Let's assume there's sufficient memory.
    autorun = malloc( exist + len );
    RegQueryValueEx( cmdkey, AUTORUN, NULL, &type, (PBYTE)autorun, &exist );
    // Remove the existing command, if present.
    ansirun = wcsstr( autorun, L"(if %ANSICON_VER%" );
    if (ansirun != NULL)
    {
      LPTSTR tmp = wcschr( ansirun, '"' );      // opening quote
      tmp = wcschr( tmp + 1, '"' );             // closing quote
      tmp = wcschr( tmp + 1, ')' );             // closing bracket
      if (*++tmp == '&')
	++tmp;
      if (*tmp == '&')
	++tmp;
      if (*tmp == '\0')
      {
	if (ansirun > autorun && ansirun[-1] == '&')
	  --ansirun;
	if (ansirun > autorun && ansirun[-1] == '&')
	  --ansirun;
      }
      wcscpy( ansirun, tmp );
      exist = TSIZE((DWORD)wcslen( autorun ) + 1);
    }
    if (inst)
    {
      if (exist == sizeof(TCHAR))
	RegSetValueEx( cmdkey, AUTORUN, 0, REG_SZ, (PBYTE)ansicon, len );
      else
      {
	memmove( (PBYTE)autorun + len, autorun, exist );
	memcpy( autorun, ansicon, len );
	((PBYTE)autorun)[len-sizeof(TCHAR)] = '&';
	RegSetValueEx( cmdkey, AUTORUN, 0, type, (PBYTE)autorun, exist+len );
      }
    }
    else
    {
      if (exist == sizeof(TCHAR))
	RegDeleteValue( cmdkey, AUTORUN );
      else
	RegSetValueEx( cmdkey, AUTORUN, 0, type, (PBYTE)autorun, exist );
    }
    free( autorun );
  }
  RegCloseKey( cmdkey );
}


// Search each process in the snapshot for id.
BOOL find_proc_id( HANDLE snap, DWORD id, LPPROCESSENTRY32 ppe )
{
  BOOL fOk;

  ppe->dwSize = sizeof(PROCESSENTRY32);
  for (fOk = Process32First( snap, ppe ); fOk; fOk = Process32Next( snap, ppe ))
    if (ppe->th32ProcessID == id)
      break;

  return fOk;
}


// Obtain the process identifier of the parent process.
BOOL GetParentProcessInfo( LPPROCESS_INFORMATION ppi, LPTSTR name )
{
  HANDLE hSnap;
  PROCESSENTRY32 pe;
  BOOL	 fOk;

  hSnap = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
  if (hSnap == INVALID_HANDLE_VALUE)
  {
    DEBUGSTR( 1, L"Failed to create snapshot (%lu)", GetLastError() );
    return FALSE;
  }

  fOk = (find_proc_id( hSnap, GetCurrentProcessId(), &pe ) &&
	 find_proc_id( hSnap, pe.th32ParentProcessID, &pe ));
  CloseHandle( hSnap );
  if (!fOk)
  {
    DEBUGSTR( 1, L"Failed to locate parent" );
    return FALSE;
  }

  ppi->dwProcessId = pe.th32ProcessID;
  wcscpy( name, pe.szExeFile );

  return fOk;
}


// Return the first non-space character from arg.
LPTSTR skip_spaces( LPTSTR arg )
{
  while (*arg == ' ' || *arg == '\t')
    ++arg;

  return arg;
}


// Retrieve an argument from the command line.	cmd gets the existing argv; argv
// is ready for the next argument.
void get_arg( LPTSTR arg, LPTSTR* argv, LPTSTR* cmd )
{
  LPTSTR line;

  line = *cmd = skip_spaces( *argv );
  while (*line != '\0')
  {
    if (*line == ' ' || *line == '\t')
    {
      ++line;
      break;
    }
    if (*line == '"')
    {
      while (*++line != '\0')
      {
	if (*line == '"')
	{
	  ++line;
	  break;
	}
	*arg++ = *line;
      }
    }
    else
      *arg++ = *line++;
  }
  *arg = '\0';
  *argv = line;
}


int glob_sort( const void* a, const void* b )
{
  return lstrcmpi( *(LPCTSTR*)a, *(LPCTSTR*)b );
}


// As get_arg, but expand wildcards.
void get_file( LPTSTR arg, LPTSTR* argv, LPTSTR* cmd )
{
  HANDLE  fh, in;
  WIN32_FIND_DATA fd;
  LPTSTR  path;
  int	  size;
  char	  buf[1024];
  static LPTSTR  name;
  static LPTSTR* glob;
  static int	 globbed;

  if (globbed != 0)
  {
    if (glob[globbed] == NULL)
    {
      free( glob );
      globbed = 0;
    }
    else
    {
      wcscpy( name, glob[globbed++] );
      return;
    }
  }

  get_arg( arg, argv, cmd );
  if (wcspbrk( arg, L"*?" ) != NULL)
  {
    fh = FindFirstFile( arg, &fd );
    if (fh != INVALID_HANDLE_VALUE)
    {
      size = 0;
      do
      {
	if (! (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
				      FILE_ATTRIBUTE_HIDDEN)))
	{
	  ++globbed;
	  size += (int)wcslen( fd.cFileName ) + 1;
	}
      } while (FindNextFile( fh, &fd ));
      FindClose( fh );

      if (globbed != 0)
      {
	for (path = name = arg; *path != '\0'; ++path)
	  if (*path == '\\' || *path == '/')
	    name = path + 1;
	glob = malloc( (globbed + 1) * PTRSZ + TSIZE(size) );
	path = (LPTSTR)(glob + globbed + 1);
	globbed = 0;
	fh = FindFirstFile( arg, &fd );
	do
	{
	  if (! (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
					FILE_ATTRIBUTE_HIDDEN)))
	  {
	    // Ignore apparent binary files.
	    wcscpy( name, fd.cFileName );
	    in = CreateFile( arg, GENERIC_READ,
			     FILE_SHARE_READ|FILE_SHARE_WRITE,
			     NULL, OPEN_EXISTING, 0, NULL );
	    if (in != INVALID_HANDLE_VALUE)
	    {
	      ReadFile( in, buf, sizeof(buf), (LPVOID)&size, NULL );
	      CloseHandle( in );
	      if (memchr( buf, 0, size ) != NULL)
		continue;
	    }
	    size = (int)wcslen( fd.cFileName ) + 1;
	    memcpy( path, fd.cFileName, TSIZE(size) );
	    glob[globbed++] = path;
	    path += size;
	  }
	} while (FindNextFile( fh, &fd ));
	FindClose( fh );
	glob[globbed] = NULL;

	qsort( glob, globbed, PTRSZ, glob_sort );

	wcscpy( name, glob[0] );
	globbed = 1;
      }
    }
  }
}


// VC macros don't like preprocessor statements mixed with strings.
#ifdef _WIN64
#define WINTYPE L"Windows"
#else
#define WINTYPE L"Win32"
#endif

void help( void )
{
  _putws(
L"ANSICON by Jason Hood <jadoxa@yahoo.com.au>.\n"
L"Version " PVERS L" (" PDATE L").  Freeware.\n"
L"http://ansicon.adoxa.vze.com/\n"
L"\n"
L"Process ANSI escape sequences in " WINTYPE L" console programs.\n"
L"\n"
L"ansicon [-l<level>] [-i] [-I] [-u] [-U] [-m[<attr>]] [-p]\n"
L"        [-e|E string | -t|T [file(s)] | program [args]]\n"
L"\n"
L"  -l\t\tset the logging level (1=process, 2=module, 3=function,\n"
L"    \t\t +4=output, +8=append) for program (-p is unaffected)\n"
L"  -i\t\tinstall - add ANSICON to CMD's AutoRun entry (also implies -p)\n"
L"  -u\t\tuninstall - remove ANSICON from the AutoRun entry\n"
L"  -I -U\t\tuse local machine instead of current user\n"
L"  -m\t\tuse grey on black (\"monochrome\") or <attr> as default color\n"
L"  -p\t\thook into the parent process\n"
L"  -e\t\techo string\n"
L"  -E\t\techo string, don't append newline\n"
L"  -t\t\tdisplay files (\"-\" for stdin), combined as a single stream\n"
L"  -T\t\tdisplay files, name first, blank line before and after\n"
L"  program\trun the specified program\n"
L"  nothing\trun a new command processor, or display stdin if redirected\n"
L"\n"
L"<attr> is one or two hexadecimal digits; please use \"COLOR /?\" for details.\n"
L"It may start with '-' to reverse foreground and background (but not for -p)."
	);
}
