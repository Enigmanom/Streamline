# FSR Gamma 2.2 compute-queue regression

Run from a Visual Studio developer shell on a GPU supported by the FSR 4 ML provider:

```powershell
.\tests\run-fsr-gamma-queue-regression.ps1 -SdkDirectory <runtime-directory>
```

The runtime directory must contain matching Streamline and FidelityFX DLLs. The
runner checks the required files, compiles the test with `/W4 /WX`, and runs each
case in a separate process. It stops at the first failure and saves a log per case.

Each case submits 96 FG frames at 2560x1440, with 1706x960 depth/motion inputs and
two presentation buffers. The test waits for the SDK's host-input completion fence
before reusing resources and checks the D3D12 device status after each presentation.

Cases:

- FSR 4 Gamma 2.2 at 166.000015 ms, starting at frame 4726: captured load-transition inputs.
- FSR 4 Gamma 2.2 at 16.666 ms, starting at frame 1.
- FSR 3 Gamma 2.2 control.
- FSR 4 sRGB control.

The original Gamma 2.2 helper used `eTextureRead`, which maps to
`PIXEL_SHADER_RESOURCE`, for compute inputs and linear intermediate textures.
FG runs that helper on a compute queue. The original plugin loses its device on
the first FSR 4 Gamma 2.2 frame with `DXGI_ERROR_INVALID_CALL`; the fixed plugin
uses `eStorageRead`, which maps to `NON_PIXEL_SHADER_RESOURCE`.

The test checks submission, completion and resource reuse. It does not check image
quality or game loading. It exits after provider quiescence without running SDK
teardown, which can otherwise hide the original device failure.
