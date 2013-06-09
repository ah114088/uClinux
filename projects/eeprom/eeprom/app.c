/*
 * This is a user-space application that reads /dev/eeprom
 * and prints the read characters to stdout
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

void usage(void)
{
	printf("usage:\n");
	printf("    app -r page npages\n");
	printf("    app -w offset text\n");
	_exit(1);
}

int main(int argc, char **argv)
{
	char *app_name = argv[0];
	const char *dev_name = "/dev/eeprom";
	int ret = -1;
    int cmd = 0;
	int i, j, fd = -1;
	char buf[8];
    int addr = 0;
    size_t offset;

	if (argc < 2)
		usage();

    if (!strcmp(argv[1], "-r"))
        cmd = 1;
    if (!strcmp(argv[1], "-w")) {
        cmd = 2;
    }
    if (!cmd)
        usage();

	if (cmd == 1) {
		int page, npages;
		if (argc < 4)
        	usage();
		page = atoi(argv[2]);
		npages = atoi(argv[3]);

		if ((fd = open(dev_name, O_RDONLY)) < 0) {
			fprintf(stderr, "%s: unable to open %s: %s\n", 
				app_name, dev_name, strerror(errno));
			goto Done;
		}

		/* dummy read instead of lseek() */
        for (i=0; i< page*8; i++) {
		    if (read(fd, buf, 8) != 8) {
				fprintf(stderr, "%s: unable to seek %s to page %d\n", 
					app_name, dev_name, page);
				goto Done;
			}
			addr += 8;
		}

		for (i=0; i< 8*npages; i++) {
			if (read(fd, buf, 8) != 8) {
					fprintf(stderr, "%s: unable to read %s: %s\n", 
						app_name, dev_name, strerror(errno));
					goto Done;
			}
			printf("%04x ", addr);
			for (j=0; j<8; j++)
			   printf("%02x ", buf[j]);
			for (j=0; j<8; j++)
			   printf("%c", isprint(buf[j])?buf[j]:'.');

			printf("\n");
			addr += 8;
		}
	} else {
		int len;
		const char *text;
		if (argc < 4)
			usage();
		if ((fd = open(dev_name, O_RDWR)) < 0) {
			fprintf(stderr, "%s: unable to open %s: %s\n", 
				app_name, dev_name, strerror(errno));
			goto Done;
		}

		offset = atoi(argv[2]);
		text = argv[3];
		len = strlen(text);

		/*
		 * read 'offset' bytes as to mimic lseek(fd, offset, SEEK_SET);
		 */
#if 1
		while (offset > 0) {	
			if (offset > 8) {
                read(fd, buf, 8);
				offset -= 8;
			} else {
                read(fd, buf, offset);
				offset = 0;
			}
		}
#else
		if (offset != 0 || lseek(fd, offset, SEEK_SET) == -1) {
			fprintf(stderr, "%s: unable to lseek %s: %s\n", 
				app_name, dev_name, strerror(errno));
			goto Done;
		}
#endif
		
		if (write(fd, text, len) != len) {
			fprintf(stderr, "%s: unable to write %s: %s\n", 
				app_name, dev_name, strerror(errno));
			goto Done;
		}
	}
	ret = 0;

Done:
	if (fd >= 0) {
		close(fd);
	}
	return ret;
}
