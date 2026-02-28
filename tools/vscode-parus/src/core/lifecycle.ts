export class LifecycleQueue {
  private queue: Promise<void> = Promise.resolve();

  public enqueue(
    task: () => Promise<void>,
    onError: (error: unknown) => void
  ): Promise<void> {
    const run = async () => {
      try {
        await task();
      } catch (error) {
        onError(error);
      }
    };
    this.queue = this.queue.then(run, run);
    return this.queue;
  }
}
