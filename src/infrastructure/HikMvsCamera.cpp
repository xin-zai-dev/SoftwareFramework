#include "HikMvsCamera.h"

#ifdef IVS_WITH_MVS
#include <MvCameraControl.h>
#endif

namespace ivs {

QString HikMvsCamera::unavailableReason() {
#ifdef IVS_WITH_MVS
    return QStringLiteral("Hikrobot MVS SDK was discovered, but the camera acquisition adapter is not "
                          "implemented or hardware-validated; backend 'mvs' is unavailable");
#else
    return QStringLiteral(
        "Hikrobot MVS camera backend is unavailable: vendor SDK and camera acquisition adapter are required. "
        "IVS_WITH_MVS currently provides an SDK integration hook only");
#endif
}

} // namespace ivs
