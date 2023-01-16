# TinyServer

Framework for building C/C++ server applications, or for quickly adding server capabilities to existing projects.

It aims at providing a simple yet powerful library for building server programs. Simplicity is at the core of it, so while a lot of fine-grained tuning is unavailable, getting a server up and running takes no time. And yet, its functions are all thread-safe and work with non-blocking async IO, so programs built on it can still service a large number of connections.

## How to use?

The [tinyserver.h](src/tinyserver.h) header file contains the structs and function declarations, and the corresponding .c file contains the implementations.

The library files can be moved in whole to the project directory, in which case the header files will include the .c files in the translation unit. They have very short compile times, which makes this approach feasible.

Alternatively, one can build these into objects or static library, in which case passing `TINYSERVER_STATIC_LINKING` as a preprocessing symbol when compiling the project will prevent the header files from including the implementation files.

## Protocol modules

Aside from the base IO capabilities, this repository also provides helper files for dealing with specific protocols. Currently the HTTP protocol is supported in [tinyserver-http.h](src/tinyserver-http.h), handling HTTP 0.9, 1.0 and 1.1. Support for HTTP 2.0, as well as other protocols, is planned.

## Dependencies

This library currently builds on top of [TinyBase](https://github.com/robertofig85/TinyBase), using tinybase-types, tinybase-memory and tinybase-platform for the IO module, and tinybase-strings for the HTTP module.

## License

MIT open source license.