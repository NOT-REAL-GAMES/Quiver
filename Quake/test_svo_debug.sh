#!/bin/bash
# Run the engine with test_room.map compile to get SVO debug output

cd /home/tremor/Desktop/Quiver/Quake

# Create a config that runs kd_compile then quits
cat > /tmp/svo_test.cfg << 'EOF'
kd_compile test_room.map
wait
quit
EOF

# Run engine with the config
./vkquake -basedir /home/tremor/Desktop/Quiver +exec /tmp/svo_test.cfg 2>&1 | grep -E "(Brush|Side|SVO|Building|bounds)"
