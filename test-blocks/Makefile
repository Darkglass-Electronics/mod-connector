
PLUGINS = test1in1out test2in2out testsidein testsideout

PREFIX ?= /usr

CFLAGS += -Wall -Wextra -std=gnu11 -fPIC
CFLAGS += -O3 -fvisibility=hidden
LDFLAGS += -Wl,--no-undefined

LV2_CFLAGS = $(shell pkg-config --cflags lv2)
LV2_LDFLAGS = $(shell pkg-config --libs lv2)

TARGETS = \
	$(PLUGINS:%=build/mod-connector-%.lv2/manifest.ttl) \
	$(PLUGINS:%=build/mod-connector-%.lv2/plugin.ttl) \
	$(PLUGINS:%=build/mod-connector-%.lv2/plugin.so)

all: $(TARGETS)

clean:
	rm -rf build

build/mod-connector-%.lv2/manifest.ttl:
	mkdir -p $(@D)
	echo "<urn:mod-connector:$*>" > $@
	echo " a <http://lv2plug.in/ns/lv2core#Plugin>;" >> $@
	echo " <http://www.w3.org/2000/01/rdf-schema#seeAlso> <plugin.ttl>." >> $@

build/mod-connector-%.lv2/plugin.ttl: ttl/%.ttl
	mkdir -p $(@D)
	install -m 644 $< $@

build/mod-connector-%.lv2/plugin.so: src/%.c
	mkdir -p $(@D)
	$(CC) $< $(CFLAGS) $(LV2_CFLAGS) $(LDFLAGS) $(LV2_LDFLAGS) -shared -o $@

build/mod-connector-%.lv2/plugin.so: src/%.cpp
	mkdir -p $(@D)
	$(CXX) $< $(CXXFLAGS) $(LV2_CFLAGS) $(LDFLAGS) $(LV2_LDFLAGS) -shared -o $@

.PHONY:
