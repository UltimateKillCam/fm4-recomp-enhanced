# Neural Rendering compatibility source

Modified 2026-10-23.

The two `nvngx_forwarder` source files are copied from
[Randolin/dlss5-wow-sidecar-revised](https://github.com/Randolin/dlss5-wow-sidecar-revised)
at commit `0fd78c4a137f0d2e7755197c44cbedb120945a15` under its MIT license. See
[the retained license](../../licenses/dlss5-sidecar-mit.txt). The copied source
credits Dagherbou/OptiScaler_DLSSNR for the caller-module compatibility finding.
The FM4 host integration is a separate GPL-3.0-only modification.

The NVIDIA NGX headers/libraries and Neural Rendering runtime are independent
components under their respective terms. They are not included in the source
archive or repository. Community runtimes are separate dependencies. The compatibility path is
experimental; NVIDIA support and completed FM4 validation are not claimed.
