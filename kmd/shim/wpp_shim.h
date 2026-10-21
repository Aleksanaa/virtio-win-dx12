// SPDX-License-Identifier: MIT
// Force-included into every kernel driver source: WPP's generated .tmh files are empty stubs in our build, so its
// init/cleanup macros become no-ops. Logging goes through yttrium's DbgPrint (kdebugprint) path, which needs no WPP.
#pragma once
#define WPP_INIT_TRACING(driver, regpath) ((void)(driver), (void)(regpath))
#define WPP_CLEANUP(driver) ((void)(driver))
