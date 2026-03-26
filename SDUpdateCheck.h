// 联网检查更新：定期拉取最新版本号（结果存偏好 uc.latest），同时上报本机版本信息。
// 失败静默重试，绝不影响主功能；%ctor 末尾调用 SDUpdateCheckStart 即可。

#pragma once

void SDUpdateCheckStart(void);
