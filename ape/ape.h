#ifndef COSMOPOLITAN_APE_APE_H_
#define COSMOPOLITAN_APE_APE_H_

#define APE_VERSION_MAJOR 1
#define APE_VERSION_MINOR 8
#define APE_VERSION_STR   APE_VERSION_STR_(APE_VERSION_MAJOR, APE_VERSION_MINOR)
#define APE_VERSION_NOTE  APE_VERSION_NOTE_(APE_VERSION_MAJOR, APE_VERSION_MINOR)

#define APE_VERSION_STR__(x, y) #x "." #y
#define APE_VERSION_STR_(x, y)  APE_VERSION_STR__(x, y)
#define APE_VERSION_NOTE_(x, y) (100000000 * (x) + 1000000 * (y))

#endif /* COSMOPOLITAN_APE_APE_H_ */
