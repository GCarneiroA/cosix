SET(CMAKE_SYSTEM_NAME Generic)

if(CMAKE_VERSION VERSION_LESS "3.6.0")
	INCLUDE(CMakeForceCompiler)
	CMAKE_FORCE_C_COMPILER(i686-unknown-cloudabi-cc Clang)
	CMAKE_FORCE_CXX_COMPILER(i686-unknown-cloudabi-c++ Clang)
else()
	set(CMAKE_C_COMPILER i686-unknown-cloudabi-cc)
	set(CMAKE_CXX_COMPILER i686-unknown-cloudabi-c++)
endif()

set(CMAKE_AR i686-unknown-cloudabi-ar CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB i686-unknown-cloudabi-ranlib CACHE FILEPATH "Ranlib")
set(CMAKE_OBJCOPY objcopy CACHE FILEPATH "Objcopy")
set(CMAKE_GLD_LINKER i686-elf-ld CACHE FILEPATH "Name of the linker for the kernel target")

mark_as_advanced(CMAKE_AR)
mark_as_advanced(CMAKE_RANLIB)
mark_as_advanced(CMAKE_OBJCOPY)
mark_as_advanced(CMAKE_GLD_LINKER)

# See if we can find these commands, so we can error out early if not
set(CHECK_COMMANDS "CMAKE_AR;CMAKE_RANLIB;CMAKE_OBJCOPY;CMAKE_GLD_LINKER")
set(CMDS_FOUND TRUE)
foreach(CMD IN LISTS CHECK_COMMANDS)
	find_program(PROGRAM_VAR ${${CMD}})
	if(NOT PROGRAM_VAR)
		message("\n${CMD} not found: ${${CMD}}.")
		message("Ensure ${${CMD}} is in your PATH or supply a path or name using -D${CMD}\n")
		set(CMDS_FOUND FALSE)
	endif()
	unset(PROGRAM_VAR CACHE)
endforeach()

if(NOT CMDS_FOUND)
	message(SEND_ERROR "Missing required tools")
endif()

execute_process(COMMAND "${CMAKE_GLD_LINKER}" "--version" OUTPUT_VARIABLE CMAKE_GLD_LINKER_VERSION)
if(CMAKE_GLD_LINKER_VERSION MATCHES "^LLD ")
	set(CMAKE_GLD_LINKER_IS_LLD TRUE)
	set(CMAKE_GLD_LINKER_IS_BINUTILS FALSE)
else()
	set(CMAKE_GLD_LINKER_IS_LLD FALSE)
	set(CMAKE_GLD_LINKER_IS_BINUTILS TRUE)
endif()

set(CMAKE_VDSO_MODULE_LINKER ${CMAKE_GLD_LINKER})
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_GLD_LINKER} <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

set(C_AND_CXX_FLAGS "-ffreestanding -O0 -g -mno-sse -mno-mmx -fno-sanitize=safe-stack -Wno-reserved-id-macro")
set(CMAKE_CXX_FLAGS_INIT "${C_AND_CXX_FLAGS} -fno-exceptions")
set(CMAKE_C_FLAGS_INIT "${C_AND_CXX_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-O0 -g -nostdlib -melf_i386")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-O0 -g -melf_i386")

set(CMAKE_VDSO_CREATE_SHARED_MODULE "${CMAKE_GLD_LINKER} ${CMAKE_MODULE_LINKER_FLAGS_INIT} <CMAKE_SHARED_LIBRARY_ASM_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> <CMAKE_SHARED_LIBRARY_CREATE_ASM_FLAGS> <SONAME_FLAG><TARGET_SONAME> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>")

set(CMAKE_C_COMPILER_FORCED TRUE)
set(CMAKE_CXX_COMPILER_FORCED TRUE)
