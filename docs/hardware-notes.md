# FCCH Air Compressor Controller Hardware Notes

## Air Tank Output Valve

There is a valve on the output port of the air compressor tank. This allows
the controller to turn off the output to the air pipes when there is no call
for air. This is done so any leaks in the pipes are less of an issue.

The valve has 2 valve control signals. These may be connected to 0V/12V or
12V/0V to open/close the valve respectively.

The vale has 2 feedback signals which indicate whether the valve is fully
open or closed. One (or neither) of these will be connected to the common
signal depending on the position of the valve.

The valve hardware is: U.S. Solid 3/4" Motorized Ball Valve stainless steel,
available at:
* [US Solid](https://ussolid.com/products/u-s-solid-motorized-ball-valve-3-4-stainless-steel-electrical-ball-valve-with-full-port-9-24-v-dc-5-wire-setup-html).
* [Amazon](https://www.amazon.com/dp/B06Y11B8VN).

Wiring:

```
Signal     Valve wire    Controller wire
Common     Black         Shield
Valve 1    Blue          Black
Valve 2    Yellow        White
Closed     Red           Red
Open       Green         Green
```
