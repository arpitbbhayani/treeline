import argparse
import ycsbr_py as ycsbr


class DatasetExtractor(ycsbr.DatabaseInterface):
    """
    A YCSBR database interface that records the workload's load dataset, read
    key trace, and update key trace.
    """

    def __init__(self):
        ycsbr.DatabaseInterface.__init__(self)
        self.dataset = []

    def initialize_database(self):
        pass

    def shutdown_database(self):
        pass

    def bulk_load(self, load):
        for i in range(len(load)):
            self.dataset.append(load.get_key_at(i))

    def insert(self, key, val):
        return True

    def update(self, key, val):
        return True

    def read(self, key):
        return None

    def scan(self, start, amount):
        return []


def extract_workload(workload_config_file, record_size_bytes):
    db = DatasetExtractor()
    print("Loading workload...")
    workload = ycsbr.PhasedWorkload.from_file(
        workload_config_file, set_record_size_bytes=record_size_bytes
    )
    print("Starting session...")
    session = ycsbr.Session(num_threads=1)
    session.set_database(db)
    session.initialize()
    try:
        print("Running bulk load...")
        session.replay_bulk_load_trace(workload.get_load_trace())
        print("Done.")
        return db
    finally:
        session.terminate()


def main():
    # Use this script to dump the dataset generated by YCSBR.
    # This script is meant to be called manually.
    parser = argparse.ArgumentParser()
    parser.add_argument("workload_config", type=str)
    parser.add_argument("--output_file", type=str, required=True)
    args = parser.parse_args()

    db = extract_workload(args.workload_config, record_size_bytes=16)
    dataset = db.dataset
    dataset.sort()

    with open(args.output_file, "w") as file:
        for key in dataset:
            print(key, file=file)


if __name__ == "__main__":
    main()