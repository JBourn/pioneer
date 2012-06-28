#include "LuaUtils.h"
#include "libs.h"
#include "FileSystem.h"
#include "lookup3.h"

static int _ro_table_error(lua_State *l)
{
	luaL_error(l, "Attempt to modify read-only table");
	return 0;
}

void pi_lua_table_ro(lua_State *l)
{
	lua_newtable(l);
	lua_pushstring(l, "__index");
	lua_pushvalue(l, -3);
	lua_rawset(l, -3);
	lua_pushstring(l, "__newindex");
	lua_pushcfunction(l, _ro_table_error);
	lua_rawset(l, -3);
	lua_pushstring(l, "__metatable");
	lua_pushboolean(l, false);
	lua_rawset(l, -3);
	lua_setmetatable(l, -2);
}

static int l_hash_random(lua_State *L)
{
	int numargs = lua_gettop(L);
	uint32_t hashA = 0, hashB = 0;

	luaL_checkany(L, 1);
	switch (lua_type(L, 1)) {
		case LUA_TNIL:
			// random numbers!
			hashA = 0xBF42B131u;
			hashB = 0x2A40F7F2u;
			break;
		case LUA_TSTRING:
		{
			size_t sz;
			const char *str = lua_tolstring(L, 1, &sz);
			lookup3_hashlittle2(str, sz, &hashA, &hashB);
			break;
		}
		case LUA_TNUMBER:
		{
			lua_Number n = lua_tonumber(L, 1);
			assert(!is_nan(n));
			lookup3_hashlittle2(&n, sizeof(n), &hashA, &hashB);
			break;
		}
		default: return luaL_error(L, "expected a string or a number for argument 1");
	}

	// generate a value in the range 0 <= x < 1
	double x = (hashA >> 5) * 67108864.0 + double(hashB >> 6);
	x *= 1.0 / 9007199254740992.0;
	if (numargs == 1) {
		// return a value x: 0 <= x < 1
		lua_pushnumber(L, x);
		return 1;
	} else {
		int m, n;
		if (numargs == 3) {
			m = lua_tointeger(L, 2);
			n = lua_tointeger(L, 3);
		} else if (numargs == 2) {
			m = 1;
			n = lua_tointeger(L, 2);
		} else {
			assert(numargs > 3);
			return luaL_error(L, "unknown argument to hash_random");
		}
		// return a value x: m <= x <= n
		lua_pushinteger(L, m + int(x * (n - m + 1)));
		return 1;
	}
}

static const luaL_Reg STANDARD_LIBS[] = {
	{ "_G", luaopen_base },
	{ LUA_COLIBNAME, luaopen_coroutine },
	{ LUA_TABLIBNAME, luaopen_table },
	{ LUA_STRLIBNAME, luaopen_string },
	{ LUA_BITLIBNAME, luaopen_bit32 },
	{ LUA_MATHLIBNAME, luaopen_math },
	{ LUA_DBLIBNAME, luaopen_debug },
	{ 0, 0 }
};

// excluded standard libraries:
//  - package library: because we don't want scripts to load Lua code,
//    or (worse) native dynamic libraries from arbitrary places on the system
//    We want to be able to restrict library loading to use our own systems
//    (for safety, and so that the FileSystem abstraction isn't bypassed, and
//    so that installable mods continue to work)
//  - io library: we definitely don't want Lua scripts to be able to read and
//    (worse) write to arbitrary files on the host system
//  - os library: we definitely definitely don't want Lua scripts to be able
//    to run arbitrary shell commands, or rename or remove files on the host
//    system
//  - math.random/math.randomseed: these use the C library RNG, which is not
//    guaranteed to be the same across platforms and is often low quality.
//    Also, global RNGs are almost never a good idea because they make it
//    almost impossible to produce robustly repeatable results
//  - dofile(), loadfile(), require(): same reason as the package library

// extra/custom functionality:
//  - math.rad is aliased as math.deg2rad: I prefer the explicit name
//  - math.hash_random(): a repeatable, safe, hash function based source of
//    variation

void pi_lua_open_standard_base(lua_State *L)
{
	for (const luaL_Reg *lib = STANDARD_LIBS; lib->func; ++lib) {
		luaL_requiref(L, lib->name, lib->func, 1);
		lua_pop(L, 1);
	}

	lua_pushnil(L);
	lua_setglobal(L, "dofile");
	lua_pushnil(L);
	lua_setglobal(L, "loadfile");

	// standard library adjustments (math library)
	lua_getglobal(L, LUA_MATHLIBNAME);
	// remove math.random and math.randomseed
	lua_pushnil(L);
	lua_setfield(L, -2, "random");
	lua_pushnil(L);
	lua_setfield(L, -2, "randomseed");
	// alias math.deg2rad = math.rad
	lua_getfield(L, -1, "rad");
	assert(lua_isfunction(L, -1));
	lua_setfield(L, -2, "deg2rad");
	// define math.hash_random which is a bit safer than math.randomseed/math.random
	lua_pushcfunction(L, &l_hash_random);
	lua_setfield(L, -2, "hash_random");
	lua_pop(L, 1); // pop the math table
}

static int l_handle_error(lua_State *L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, "PiDebug");
	lua_getfield(L, -1, "error_handler");
	lua_pushvalue(L, 1);
	lua_pcall(L, 1, 1, 0);
	return 1;
}

int pi_lua_panic(lua_State *L)
{
	luaL_where(L, 0);
	std::string errorMsg = lua_tostring(L, -1);
	lua_pop(L, 1);

	errorMsg += lua_tostring(L, -1);
	lua_pop(L, 1);

	lua_getglobal(L, "debug");
	lua_getfield(L, -1, "traceback");
	lua_call(L, 0, 1);
	errorMsg += "\n";
	errorMsg += lua_tostring(L, -1);
	errorMsg += "\n";
	Error("%s", errorMsg.c_str());
	// Error() is noreturn

	// XXX when Lua management is good enough, we can probably remove panic
	//     entirely in favour of pcall and a nicer error handling system
	RETURN_ZERO_NONGNU_ONLY;
}

void pi_lua_protected_call(lua_State* L, int nargs, int nresults) {
	int handleridx = lua_gettop(L) - nargs;
	lua_pushcfunction(L, &l_handle_error);
	lua_insert(L, handleridx);
	int ret = lua_pcall(L, nargs, nresults, handleridx);
	lua_remove(L, handleridx); // pop error_handler
	if (ret) {
		std::string errorMsg = lua_tostring(L, -1);
		lua_pop(L, 1);
		Error("%s", errorMsg.c_str());
	}
}

static void pi_lua_dofile(lua_State *l, const FileSystem::FileData &code)
{
	assert(l);
	LUA_DEBUG_START(l);
	// XXX make this a proper protected call (after working out the implications -- *sigh*)
	lua_pushcfunction(l, &pi_lua_panic);
	if (luaL_loadbuffer(l, code.GetData(), code.GetSize(), code.GetInfo().GetPath().c_str())) {
		pi_lua_panic(l);
	} else {
		int ret = lua_pcall(l, 0, 0, -2);
		if (ret) {
			const char *emsg = lua_tostring(l, -1);
			if (emsg) { fprintf(stderr, "lua error: %s\n", emsg); }
			switch (ret) {
				case LUA_ERRRUN:
					fprintf(stderr, "Lua runtime error in pi_lua_dofile('%s')\n",
							code.GetInfo().GetAbsolutePath().c_str());
					break;
				case LUA_ERRMEM:
					fprintf(stderr, "Memory allocation error in Lua pi_lua_dofile('%s')\n",
							code.GetInfo().GetAbsolutePath().c_str());
					break;
				case LUA_ERRERR:
					fprintf(stderr, "Error running error handler in pi_lua_dofile('%s')\n",
							code.GetInfo().GetAbsolutePath().c_str());
					break;
				default: abort();
			}
			lua_pop(l, 1);
		}
	}
	lua_pop(l, 1);
	LUA_DEBUG_END(l, 0);
}

void pi_lua_dofile(lua_State *l, const std::string &path)
{
	assert(l);
	LUA_DEBUG_START(l);

	RefCountedPtr<FileSystem::FileData> code = FileSystem::gameDataFiles.ReadFile(path);
	if (!code) {
		fprintf(stderr, "could not read Lua file '%s'\n", path.c_str());
	}

	// XXX kill CurrentDirectory
	std::string dir = code->GetInfo().GetDir();
	if (dir.empty()) { dir = "."; }
	lua_pushstring(l, dir.c_str());
	lua_setglobal(l, "CurrentDirectory");

	pi_lua_dofile(l, *code);

	// XXX kill CurrentDirectory
	lua_pushnil(l);
	lua_setglobal(l, "CurrentDirectory");

	LUA_DEBUG_END(l, 0);
}

void pi_lua_dofile_recursive(lua_State *l, const std::string &basepath)
{
	LUA_DEBUG_START(l);

	for (FileSystem::FileEnumerator files(FileSystem::gameDataFiles, basepath, FileSystem::FileEnumerator::IncludeDirs); !files.Finished(); files.Next())
	{
		const FileSystem::FileInfo &info = files.Current();
		const std::string &fpath = info.GetPath();
		if (info.IsDir()) {
			pi_lua_dofile_recursive(l, fpath);
		} else {
			assert(info.IsFile());
			if ((fpath.size() > 4) && (fpath.substr(fpath.size() - 4) == ".lua")) {
				// XXX kill CurrentDirectory
				lua_pushstring(l, basepath.empty() ? "." : basepath.c_str());
				lua_setglobal(l, "CurrentDirectory");

				RefCountedPtr<FileSystem::FileData> code = info.Read();
				pi_lua_dofile(l, *code);
			}
		}
		LUA_DEBUG_CHECK(l, 0);
	}

	LUA_DEBUG_END(l, 0);
}

// XXX compatibility
int pi_load_lua(lua_State *l) {
	const std::string path = luaL_checkstring(l, 1);
	FileSystem::FileInfo info = FileSystem::gameDataFiles.Lookup(path);

	lua_getglobal(l, "CurrentDirectory");
	std::string currentDir = luaL_optstring(l, -1, "");
	lua_pop(l, 1);

	if (info.IsDir()) {
		pi_lua_dofile_recursive(l, path);
	} else if (info.IsFile() && (path.size() > 4) && (path.substr(path.size() - 4) == ".lua")) {
		pi_lua_dofile(l, path);
	} else if (info.IsFile()) {
		return luaL_error(l, "load_lua('%s') called on a file without a .lua extension", path.c_str());
	} else if (!info.Exists()) {
		return luaL_error(l, "load_lua('%s') called on a path that doesn't exist", path.c_str());
	} else {
		return luaL_error(l, "load_lua('%s') called on a path that doesn't refer to a valid file", path.c_str());
	}

	if (currentDir.empty())
		lua_pushnil(l);
	else
		lua_pushlstring(l, currentDir.c_str(), currentDir.size());
	lua_setglobal(l, "CurrentDirectory");

	return 0;
}

void pi_lua_warn(lua_State *l, const char *format, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	fprintf(stderr, "Lua Warning: %s\n", buf);

	lua_Debug info;
	int level = 0;
	while (lua_getstack(l, level, &info)) {
		lua_getinfo(l, "nSl", &info);
		fprintf(stderr, "  [%d] %s:%d -- %s [%s]\n",
			level, info.short_src, info.currentline,
			(info.name ? info.name : "<unknown>"), info.what);
		++level;
	}
}
