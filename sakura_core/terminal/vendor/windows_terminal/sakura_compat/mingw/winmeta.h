#pragma once

// The selected parser closure does not consume Windows SDK ETW metadata.
// Keep the imported tracing declaration buildable under MinGW without
// providing a fake metadata implementation.
