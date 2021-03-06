# Fletcher: A framework to integrate FPGA accelerators with Apache Arrow

Fletcher is a framework that helps to integrate FPGA accelerators with tools that use
Apache Arrow in their back-ends.

[Apache Arrow](https://arrow.apache.org/) specifies an in-memory format for data and 
provides libraries to various languages to interface with the data in that format. The
format prevents the need for serialization and through the libraries Arrow, zero-copy 
interprocess communication is possible. Languages that have Arrow libraries (under development)
include C, C++, Go, Java, JavaScript, Python, Ruby and Rust.

While many software projects can benefit from these advantages, also hardware accelerated
applications have seen serious serialization bottlenecks. Fletcher focuses on FPGA accelerators only.
Through Fletcher and Arrow, efficient FPGA acceleration is made available to all the supported languages.

<sup><sub>(If you are looking for GPGPU's, some work is being done as part of the Arrow project already, go check out 
[the Arrow repository](https://github.com/apache/arrow).)</sub></sup>

Given an Arrow Schema (description of a tabular datastructure), Fletcher generates the following:

* An easy-to-use hardware interface for the functional part of the accelerator
  * You provide a range of table indices (rather than byte address)
  * You receive streams of the datatype specified in the schema (rather than bus words)
  * No pointer arithmetic, reordering, parallelizing or serializing, buffering, etc.. required!
* A template for the functional part of accelerator (to be implemented manually or with HLS)

<img src="fletcher.svg">

## Current state
Our framework is functional, but in early stages of development. It currently supports the
following features:

### Apache Arrow support:
* Reading/writing an Arrow Schema created from any combination of:
  - Fixed-width primitives
  - Lists
  - Structs
* Validity bitmaps are supported.
* Not supported (yet):
  - Unions
  - Dictionaries
  - Chunks

### Platform support:
* Our core hardware descriptions are vendor independent; __we don't use any vendor IP__.
* There is an example project for the Amazon EC2 F1 and POWER8/9 CAPI SNAP FPGA platforms.
* Our bus interconnect is similar to AXI, so it should be easy to integrate in many existing 
  platforms.
* Upcoming: OpenCAPI support

## Further reading
  * [How to generate a Column Reader](hardware). 
    * For the current state of the project, this would be your starting
      point in hardware design.
  * [How to use the host-side run-time library](runtime).
  * [Regular Expression matching example on Amazon EC2 F1](platforms/aws-f1/regexp)
  * [Regular Expression matching example on CAPI SNAP](platforms/snap/regexp)
