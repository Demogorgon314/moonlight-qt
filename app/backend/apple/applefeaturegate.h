#pragma once

class AppleFeatureGate
{
public:
    // This gate is deliberately read-only in stage 1. Enabling it only makes
    // verified Apple capabilities eligible for registration; stage 1 registers none.
    static bool isRuntimeEnabled();
};

