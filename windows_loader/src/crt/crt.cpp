// ntoskrnl.lib already provides memcpy, memmove, memcmp and strlen, so
// this CRT deliberately does not define them - doing so made the link
// order-sensitive and produced duplicate symbol errors against
// ntoskrnl.lib. Only symbols the kernel does not export belong here.

extern "C" {
void __cxa_pure_virtual()
{
}
}
