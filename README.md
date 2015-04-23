# RestoreWindows
A process that tracks window placements and restores them when display connections are back to normal

When a monitor connected with either DisplayPort or HDMI is turned off, it's disconnected from Windows
as if the cable was removed (this behaviour depends on both monitor model, graphics card, and drivers)

This utility continually tracks the position and size of desktop windows and restores them to their
correct placement when all monitors are reconnected to the system.

Usage:
  RestoreWindow [OPTIONS]
  
  --delay t       The time in milliseconds between display reconnect and window restoration. Defaults to 1000
  --debuglog      Writes a debug log to RestoreWindow.log

