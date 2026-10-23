#ifndef SH_RESOURCE_MD6_READS_H
#define SH_RESOURCE_MD6_READS_H
/* Actual model/skeleton use at the verified MD6 Parse mesh return sites. */
int sh_resource_md6_reads_install(const unsigned char *base,int (*observe)(void *));
int sh_resource_md6_reads_ready(void);
#endif
