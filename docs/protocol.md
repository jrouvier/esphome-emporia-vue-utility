# Protocol details

Each message sent or received begins with a `$` (hex 24).  Messages end with a carrage return (`\r` / hex 0D).

### Message types

| Msg char | Hex value | Description |
| -------- | --------- | ----------- |
| r        |  0x72  | Get meter reading |
| j        |  0x6a  | Join the meter |
| m        |  0x6d  | Get mac address (of the MGM111)|
| i        |  0x69  | Get install code |
| f        |  0x66  | Get firmware version |

## Sending messages

Messages from the ESP to the MGM111 have just the previously mentioned `$` starting delimiter, then a message type character and the 
ending `\r` delimiter.  Each message is therefore exactly 3 bytes long.  For example, to get the a mac address, send the hex bytes
`24 6D 0D` (`$m\r`)

## Receiving responses

Response format bytes:

|  0 |  1 |  2 |  3 | ... | x |
| -- | -- | -- | -- | --- | - |
| `$` | 0x01 | *\<msg type\>* | *\<payload length\>* | *\<payload\>...* | `\r` |

Byte 1, 0x01 indicates that this is a response
Byte 2 is the same messages type that was used to trigger this response
Byte 3 is the length of the payload that follows
Bytes 4+ are the payload of the response
The final byte is the terminator

Ex:  
`24 01 6D 08 11 22 33 44 55 66 77 88 0D`

#### Mac address response payload
The mac address reponse bytes are in reverse order, if the device responds with `11 22 33 44 55 66 77 88` then the mac address is
`88:77:66:55:44:33:22:11`

#### Install code response payload
The install code bytes are not swapped like the mac address payload.

#### Meter reading response payload
The meter response payload has it's own page [protocol-meter-reading.md](protocol-meter-reading.md)
