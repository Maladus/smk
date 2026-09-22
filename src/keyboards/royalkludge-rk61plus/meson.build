kb_dir = meson.current_source_dir()

keyboards += {
    'royalkludge-rk61plus' : {
        'dir' : kb_dir,
        'platform' : 'sh68f90',
        'src' : files(
            'user_matrix.c',
            'user_init.c',
            'user_sleep.c',
            'user_battery.c',
            'kb.c',
        ),
        'layouts' : {
            'default' : {
                'dir' : kb_dir / 'layouts/default',
                'src' : files(
                    'layouts/default/indicators.c',
                    'layouts/default/layout.c',
                ),
            },
        },
        'options' : {
            'vendor_id' : '0x0603',
            'product_id' : '0x1020',
            'wireless' : 'bk3632',
            'led_rows' : '5',
            'led_cols' : '14',
            'led_cx2' : '13',
            'led_cy2' : '4',
        },
    },
}
