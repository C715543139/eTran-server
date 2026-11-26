protocols := entrance homa tcp sched

all: $(protocols)

$(protocols):
	$(MAKE) -C $@

.PHONY: clean $(protocols)
clean:
	for dir in $(protocols); do \
		$(MAKE) -C $$dir clean; \
	done
