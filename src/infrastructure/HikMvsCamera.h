#pragma once

#include <QString>

namespace ivs {

// Integration hook only. SDK discovery is independent of an implemented adapter.
// Keeping this explicit prevents a simulated image being passed off as camera I/O.
class HikMvsCamera final {
  public:
    static QString unavailableReason();
};

} // namespace ivs
