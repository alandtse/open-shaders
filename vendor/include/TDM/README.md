# True Directional Movement API

`TrueDirectionalMovementAPI.h` matches the API header from upstream commit
[`99f92e5594b66b5996768a42f7311273ae7f4128`](https://github.com/ersh1/TrueDirectionalMovement/blob/99f92e5594b66b5996768a42f7311273ae7f4128/src/TrueDirectionalMovementAPI.h),
except that `GetModuleHandle` uses its explicit `GetModuleHandleA` variant.
This version is distributed under the accompanying [MIT license](LICENSE).

The header was originally imported in Open Shaders commit `74d1c528b` and
moved here with the portability change in `ea5c6097c`.
