#pragma once

class AppleFeatureGate
{
public:
    // Deliberately read-only here: registration is decided once when the catalog
    // is constructed, before discovery or any credential access can occur.
    static bool isRuntimeEnabled();
};
