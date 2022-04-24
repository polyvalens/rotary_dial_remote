# Rotary Dial Remote

Introduction

In this project the WIZnet W5100S-EVB-Pico board is used to turn an old analog rotary-dial phone into a remote control and alarm for a home automation system.

[photo]

Even when lights and appliances are controlled by a home automation controller (HAC), it is often desirable to have an override option to switch on or off a lamp, a ventilator or some other device. The modified rotary-dial phone presented here allows this by dialing the number of the appliance. At the same time it forms a now sought-after decorative object.

Besides a remote control for a home automation system, the modified phone can also be used as a prop in for instance an escape game. I am sure many other applications can be imagined too.

The HAC in question is Home Assistant but any controller supporting MQTT can be used.

Functions

The modified rotary-dial phone has the following functions:
- When the handset is on the phone and a number is dialed, the corresponding appliance is switched on or off, depending on its current state.
- When the handset is lifted, the dial can be used to compose a text message in a way similar to mobile phones from some twenty years ago. Replacing the handset on the phone will send the message.
- The bell of the phone is available to the HAC as an alarm and can e.g. be routed to the doorbell or function as a kitchen timer or (unpleasant) wake-up alarm.
- The loudspeaker of the phone will be connected to an MP3 player to play prerecorded messages or music. This allows for implementing for instance a talking clock as was common in the previous century.

Communication between the phone and the HAC uses MQTT, while mDNS is used to establish a connection between the two.
