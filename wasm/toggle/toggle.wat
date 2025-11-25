(module
  (import "env" "gpio_toggle" (func $gpio_toggle))
  (func (export "toggle_forever")
    (loop $L
      call $gpio_toggle
      br $L)))