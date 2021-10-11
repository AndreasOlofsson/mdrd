# mdrd

A daemon that uses [libmdr](https://github.com/AndreasOlofsson/libmdr) and the Bluez stack to connect to any MDR-devices (Sony wireless headphones) and expose them on the system D-Bus.

_Please note that this project is a work in progress and that the DBus API is likely to change until it reaches v1.0._

## Building

Run `make` in the project root.

### Dependencies

* a C compiler (gcc is recommended)
* make
* pkg-config
* git (or place the correct version of [libmdr](https://github.com/AndreasOlofsson/libmdr) in the project root)
* gio-2.0
* gio-unix-2.0

