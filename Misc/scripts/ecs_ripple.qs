// One call for each stress entity on every server physics tick.
void quiver_ripple_tick(entity target, float tick_time) {
    vector position = target.origin;
    float radius = sqrt(position_x * position_x + position_y * position_y);
    position_z = 160.0 + 24.0 * sin(tick_time * 2.0943951023931953 - radius * 0.02454369260617026);
    setorigin(target, position);
}
