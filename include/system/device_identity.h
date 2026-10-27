#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H
#ifdef __cplusplus
extern "C" {
#endif
/* Stable, MAC-suffixed hostname shared by DHCP, mDNS, MQTT and the LCD. */
const char *device_identity_hostname(void);
#ifdef __cplusplus
}
#endif
#endif /* DEVICE_IDENTITY_H */
