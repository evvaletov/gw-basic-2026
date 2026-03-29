from ipykernel.kernelapp import IPKernelApp
from .kernel import GWBasicKernel

IPKernelApp.launch_instance(kernel_class=GWBasicKernel)
