(module
  (import "env" "led_toggle" (func $led_toggle (param i32))) ;; param: durata in ms
  (func (export "step")
    ;; Chiede di eseguire un lampeggio di 500ms
    i32.const 500
    call $led_toggle
  )
)