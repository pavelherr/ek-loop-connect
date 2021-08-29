# EK Loop Connect Protocol

## DISCLAIMER:
All information here was obtained by sniffing USB traffic between the controller
and the official EK software.

Not all bytes in the protocol are understood, use at your own risk.

## Interface

EK loop connect exposes 2 USB HID interfaces, but only interface 0 appears to be
used for control purposes. It is unclear what the purpose of interface 1 is.

## Request format

There appears to be 3 kinds of request/response pairs. All requests/responses
are 63 bytes long, even though many of the bytes appear to just be zero-padding.

A lot of the logic happens in the controller itself, making the driver fairly
simple, but also limiting the capabilities somewhat.

Several bytes in the protocol have been identified as checksum bytes, and their
value seems to change whenever other bytes change, without having an obvious
meaning. Luckily, the controller seems to be rather permissive, and happily
accept commands with these bytes set to 0xff.

There is an additional request/response pair used for RGB control. This is not
used or described here, but is already supported by OpenRGB project.

### Fans

The controller has 6 fan channels with independent PWM control, labelled F1-F6.

There doesn't appear to be any automatic control, instead the control software
just reads all channels every second or so, and then sets new target speed
depending on software configuration and temperatures.

#### Reading fan speed

The fan read request is comprised of the following:

| byte position | value                                  |
|---------------|----------------------------------------|
| 0 - 5         | `0x10, 0x12, 0x08, 0xaa, 0x01, 0x03`   |
| 6 - 7         | channel constant                       |
| 8 - 9         | `0x00, 0x20`                           |
| 10 - 12       | unknown, likely checksum [^read_check] |
| 13            | `0xed`                                 |
| 14 - 62       | `0x00` repeated                        |

[^read_check] Given the only variable field is the channel, one could build a
  small table with value for each channel, but the controller seems happy with
  any value. The first byte here seems to always be `0x66` or `0x67`.


The response to this request has the following format:

| byte position | value                                            |
|---------------|--------------------------------------------------|
| 0 - 7         | `0x10, 0x12, 0x27, 0xaa, 0x01, 0x03, 0x00, 0x20` |
| 8 - 11        | `0x00` repeated                                  |
| 12 - 13       | fan speed as RPM, high byte first                |
| 14 - 20       | `0x00` repeated                                  |
| 21            | PWM target, 0-100 (`0x00`-`0x64`)                |
| 22 - 39       | `0x00` repeated                                  |
| 40 - 41       | `0xaa, 0xbb`                                     |
| 42            | unknown, likely checksum                         |
| 43            | `0xed`                                           |
| 44 - 62       | `0x00` repeated                                  |


#### Setting fan speed

The fan read request is comprised of the following:

| byte position | value                                     |
|---------------|-------------------------------------------|
| 0 - 5         | `0x10, 0x12, 0x29, 0xaa, 0x01, 0x10`      |
| 6 - 7         | channel constant                          |
| 8 - 10        | `0x00, 0x10, 0x20`                        |
| 11 - 14       | `0x00` repeated                           |
| 15 - 16       | fan RPM [^fan_set_rpm]                    |
| 17 - 23       | `0x00` repeated                           |
| 24            | fan PWM target, 0-100 (`0x00`-`0x64`)     |
| 25            | unknown, likely checksum [^fan_set_check] |
| 26 - 44       | `0x00` repeated                           |
| 45            | unknown, likely checksum [^fan_set_check] |
| 46            | `0xed`                                    |
| 47 - 62       | `0x00` repeated                           |

[^fan_set_rpm] It is unclear whether setting the RPM target without specifying
  PWM target does anything. The official software sets these bytes to values
  similar to recently-read RPM value, but always setting this to 0 seems to work
  fine.

[^fan_set_check] Unlike the case of reading fan speed, the number of possible
  combinations is high, so it doesn't make sense to build a complete table.
  Again, the controller seems to be happy with anything here.

The response to this request has the following format:

| byte position | value                    |
|---------------|--------------------------|
| 0 - 2         | `0x10, 0x12, 0x06`       |
| 3 - 6         | `0x00` repeated          |
| 7 - 10        | `0xaa, 0xbb, 0x65, 0xed` |
| 11 - 62       | `0x00` repeated          |


#### Fan channels

Fan channels are identified by the following 2 byte constants.

| fan | value        |
|-----|--------------|
| F1  | `0xa0, 0xa0` |
| F2  | `0xa0, 0xc0` |
| F3  | `0xa0, 0xe0` |
| F4  | `0xa1, 0x00` |
| F5  | `0xa1, 0x20` |
| F6  | `0xa1, 0xe0` |


### Sensors

The controller has 3 temperature sensor ports, marked T1-T3, for use with
thermistors, similar to many motherboards. There is also a propriatery port
for a coolant flow sensor and a coolant level sensor.

The sensor read requst looks just like a fan read request, using channel
constant of `0xa2, 0x20`

The sensor read response has the following format:

| byte position | value                                            |
|---------------|--------------------------------------------------|
| 0 - 7         | `0x10, 0x12, 0x27, 0xaa, 0x01, 0x03, 0x00, 0x20` |
| 8 - 10        | `0x00, 0x01, 0x00`                               |
| 11            | temperature reading from T1, in degrees Celsius  |
| 12 - 14       | `0x00, 0x01, 0x00`                               |
| 15            | temperature reading from T2, in degrees Celsius  |
| 16 - 18       | `0x00, 0x01, 0x00`                               |
| 19            | temperature reading from T3, in degrees Celsius  |
| 20 - 21       | `0x00` repeated                                  |
| 22 - 23       | flow sensor reading (high byte first) [^flow]    |
| 24 - 26       | `0x00, 0x01, 0x00`                               |
| 27            | coolant level sensor reading [^level]            |
| 28 - 39       | `0x00` repeated                                  |
| 40 - 41       | `0xaa, 0xbb`                                     |
| 42            | unknown, likely checksum                         |
| 43            | `0xed`                                           |
| 44 - 63       | `0x00` repeated                                  |


[^flow] The official control software (with default configuration) seems to
  multiply this number by 0.8 and interpret as l/h

[^level] The value appears to either be `0x64` for 'optimal' or `0x00` for
  'warning'.

It is unclear whether the reported temperatures are using signed or unsigned
values. When a sensor port is not used, the corresponding byte is set to `0xe7`.

