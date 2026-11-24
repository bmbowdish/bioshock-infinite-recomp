#!/bin/bash
G="$HOME/Library/Application Support/Steam/steamapps/common/BioShock Infinite/BioShock3.app/Contents/GameData/XGame/Content"
ls "$G" 2>&1 | head; echo ==; ls "$G/Audio" 2>&1 | head; echo ==; ls "$G/Audio/Packed" 2>&1 | head; echo ==; ls "$G/Audio/Packed/"* 2>&1 | head -12
