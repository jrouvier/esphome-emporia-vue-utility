# Meter Response Payload

The meter reading response contains 152 bytes of payload, most of which seems to always be zeros.  The below table details the payload format.
Blank cells have never been seen to be anything other than zero.  Note the table is zero-indexed

<table  style="width:20%">
  <tr>   <td></td>
            <th align="center"><img width="50" height="1">0<img width="50" height="1"></th>
            <th align="center"><img width="50" height="1">1<img width="50" height="1"></th>
            <th align="center"><img width="50" height="1">2<img width="50" height="1"></th>
            <th align="center"><img width="50" height="1">3<img width="50" height="1"></th>
  </tr>
  <tr>   <th>0</th> <td colspan=4></td></tr>
  <tr>   <th>4</th> <td colspan=4 align="center">EnergyVal</td></tr>
  <tr>   <th>...</th> <td colspan=4></td></tr>
  <tr>   <th>44</th> <td colspan=3></td><td align="center">MeterDiv</td></tr>
  <tr>   <th>48</th> <td colspan=2></td><td colspan=2 align="center">EnergyCostUnit</td></tr>
  <tr>   <th>52</th> <td colspan=2 align="center">Unknown 1</td><td colspan=2></td></tr>
  <tr>   <th>56</th> <td></td><td colspan=3 align="center">PowerVal</td></tr>
  <tr>   <th>...</th> <td colspan=4></td></tr>
  <tr>   <th>148</th> <td colspan=4 align="center">MeterTS</td></tr>
</table>

## Known Fields

### EnergyVal

Bytes 4 to 8, 32 bit int, unknown if signed, MSB

Energy Meter totalizer in watts, in other words, cumulative watt-hours consumed.  Unknown when this value resets to zero, 
might reset monthly or on start of new billing cycle.

Sometimes, an invalid number greater than `0x00400000` is returned, it is not understood when or why this happens.

### MeterDiv

At least byte 47, maybe as large as bytes 44 to 47

Some meters report values not in watts and watt hours but in a multiple of those values.  `EnergyVal` and `PowerVal` should 
be divided by `MeterDiv` to determine the real value.  Usually this is 1, but have also seen a value of 3.

### EnergyCostUnit

Bytes 50 and 51 MSB(?)

Usually `0x03E8`, which is 1000.  Theorized to be how many `EvergyVal` units per "cost unit" (a value we don't appear to have).
Since people are typically charged per kWh, this value is typically 1000.

This value is not currently used in the code

### PowerVal

Bytes 57 to 59 (24 bit signed int, MSB)

The power being consumed at the moment in Watts.  If you have a grid-tie solar / wind / battery system, then this value can go negative.
Negative values are returned in 1's complement notation (if left most bit is 1, then flip all the bits then multiply by -1)

"Data Missing" / "Unknown" is denoted as max negative, `0x800000`

### MeterTS

Bytes 148 to 151 (32 bit unsigned int, LSB)

Number of milliseconds since an unknown event.  Could be time since `EnergyVal` was reset, or could just be a free-running timer.
Will roll over every 49 days.

Only reported as a debugging value, not used in calculations.

### Unknown 1

Bytes 52 and 53

The meaning of the values in these fields is completely unknown.  They appear to be static for each meter.  Some of the observed values from users include:
```
fbfb
2c2b
3133
```
Random uneducated guess is that this is a bit field with flags about the meter configuration.
