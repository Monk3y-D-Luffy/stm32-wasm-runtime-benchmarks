(module
  (import "env" "uart_print" (func $uart_print (param i32)))
  (memory 1)
  (data (i32.const 64) "Messaggio\r\n")
  (func (export "step")
    i32.const 64
    call $uart_print
  )
)
