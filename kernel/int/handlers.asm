default rel
%include "../kernel/int/int_mac.mac"

extern page_fault_handler
global page_fault_handler_wrapper
page_fault_handler_wrapper:
    pass_err_to_routine page_fault_handler

extern gpf_handler
global gpf_handler_wrapper
gpf_handler_wrapper:
    pass_err_to_routine gpf_handler

extern ss_handler
global ss_handler_wrapper
ss_handler_wrapper:
    pass_err_to_routine ss_handler

extern double_fault_handler
global double_fault_handler_wrapper
double_fault_handler_wrapper:
    pass_err_to_routine double_fault_handler
