# Animation Compression Library

Animation compression is a fundamental aspect of modern video game engines. Not only is it important to keep the memory footprint down but it is also critical to keep the animation clip sampling performance fast.

The more memory an animation clip consumes, the slower it will be to sample it and extract a character pose at runtime. For these reasons, any game that attempts to push the boundaries of what the hardware can achieve will at some point need to implement some form of animation compression.

While some degree of compression can easily be achieved with simple tricks, achieving high compression ratios, fast decompression, while simultaneously not compromising the accuracy of the resulting compressed animation requires a great deal of care.

## Goals

This library has four primary goals:

*  Implement state of the art and production ready animation compression algorithms
*  Be easy to integrate into modern video game engines
*  Serve as a benchmark to compare various techniques against one another
*  Document what works and doesn't work

Algorithms are optimized with a focus on (in this particular order):

*  Minimizing the compression artifacts in order to reach high cinematographic quality
*  Fast decompression on all our supported hardware
*  A small memory footprint to lower memory pressure at runtime as well as reducing disk and network usage

Decompression speed will not be sacrificed for a smaller memory footprint nor will accuracy be compromised under any circumstances.

## Philosophy

Much thought was put into designing the library for it to be as flexible and powerful as possible. To this end, the following decisions were made:

*  The library consists of **100% C++11** header files and is thus easy to integrate in any game engine
*  [An intermediary clip format](./docs/the_acl_file_format.md) is supported in order to facilitate debugging and bug reporting
*  All allocations use a [game provided allocator](./includes/acl/core/iallocator.h)
*  All asserts use a [game provided macro](./includes/acl/core/error.h)

## Supported platforms

*  Windows VS2015 x86 and x64
*  Windows (VS2017 to VS2022) x86, x64, and ARM64
*  Windows (VS2017 to VS2022) with clang x86 and x64
*  Linux (gcc 5 to 13) x86 and x64
*  Linux (clang 4 to 15) x86 and x64
*  OS X (12.5, 13.2, 14.2) x64 and ARM64
*  Android (NDK 21) ARMv7-A and ARM64
*  iOS (Xcode 10.3, 11.7, 12.5, 13.2, 14.2) ARM64
*  Emscripten (1.39.11) WASM

The above supported platform list is only what is tested every release but if it compiles, it should run just fine.

The [Unreal Engine](https://www.unrealengine.com/en-US/blog) is supported through a plugin found [here](https://github.com/nfrechette/acl-ue4-plugin).

## Getting started

This library is **100%** headers as such you just need to include them in your own project to start using it. However, if you wish to run the unit tests, regression tests, to contribute to ACL or use it for research, head on over to the [getting started](./docs/getting_started.md) section in order to setup your environment and make sure to check out the [contributing guidelines](CONTRIBUTING.md).

If you would like to integrate ACLIB into your own game engine, follow the integration instructions [here](./docs#how-to-integrate-the-library).

You can install `aclib` with Conan.

## Performance metrics

*  [Carnegie-Mellon University database performance](./docs/cmu_performance.md)
*  [Paragon database performance](./docs/paragon_performance.md)
*  [Matinee fight scene performance](./docs/fight_scene_performance.md)
*  [Decompression performance](./docs/decompression_performance.md)

## External dependencies

You don't need anything else to get started: everything is self contained.
See [here](./external) for details.

## License, copyright, and code of conduct

This project uses the [MIT license](LICENSE).

Copyright (c) 2017 Nicholas Frechette & Animation Compression Library contributors

Please note that this project is released with a [Contributor Code of Conduct](CODE_OF_CONDUCT.md). By participating in this project you agree to abide by its terms.

This project follows the [all-contributors](https://github.com/all-contributors/all-contributors) specification. Contributions of any kind welcome!
