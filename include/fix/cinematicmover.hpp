#pragma once

namespace kraken::fix::cinematicmover {
    void OnBodyBeforeStep(void* body, float stepTime);
    void Apply();
}
