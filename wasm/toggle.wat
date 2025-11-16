(module
  (import "env" "gpio_toggle" (func $gpio_toggle))
  (func (export "toggle_forever")
    (loop $L
      call $gpio_toggle
      br $L))
  (func (export "toggle_n") (param $n i32)
    (local $i i32)
    (loop $R
      call $gpio_toggle
      local.get $i
      i32.const 1
      i32.add
      local.tee $i
      local.get $n
      i32.lt_u
      br_if $R)))
