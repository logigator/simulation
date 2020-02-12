#include <thread>
#include <chrono>
#include <algorithm>
#include "fast_stack.h"
#include "board.h"
#include "component.h"
#include "link.h"
#include "spinlock_barrier.h"
#include "events.h"
#include "output.h"

Board::Board() = default;

Board::~Board()
{
	stop();

	for (unsigned int i = 0; i < componentCount; i++) {
		delete components[i];
	}

	if (componentCount > 0)
		delete[] components;

	if (linkCount > 0)
		delete[] links;

    #ifndef __EMSCRIPTEN__

	for (unsigned int i = 0; i < threadCount; i++) {
		delete threads[i];
	}
	delete[] threads;
	delete barrier;

	#endif

	delete[] linkStates;
	delete readBuffer;
	delete writeBuffer;
}

#ifdef __EMSCRIPTEN__

void Board::init(Component** components, Link* links, const unsigned int componentCount, const unsigned int linkCount)
{
	if (currentState != Board::Uninitialized)
		return;

	this->components = components;
	this->links = links;
	this->componentCount = componentCount;
	this->linkCount = linkCount;

	if (linkCount > 0)
		this->linkStates = new bool[linkCount] { false };
	else
		this->linkStates = new bool[0];
	    
	for (unsigned int i = 0; i < linkCount; i++) {
		links[i].powered = &this->linkStates[i];
	}

	currentState = Board::Stopped;
	lastCapture = std::chrono::high_resolution_clock::now();

	for (unsigned int i = 0; i < componentCount; i++)
	{
		components[i]->init();
	}

	auto* readBuffer = this->readBuffer;
	this->readBuffer = this->writeBuffer;
	this->writeBuffer = readBuffer;
	this->writeBuffer->clear();
}

#else

void Board::init(Component** components, Link* links, const unsigned int componentCount, const unsigned int linkCount)
{
	if (currentState != Board::Uninitialized)
		return;

	this->components = components;
	this->links = links;
	this->componentCount = componentCount;
	this->linkCount = linkCount;

	if (linkCount > 0)
		this->linkStates = new bool[linkCount] { false };
	else
		this->linkStates = new bool[0];

	for (unsigned int i = 0; i < linkCount; i++)
	{
		links[i].powered = &this->linkStates[i];
	}
	
	currentState = Board::Stopped;
	lastCapture = std::chrono::high_resolution_clock::now();

	for (unsigned int i = 0; i < componentCount; i++)
	{
		components[i]->init();
	}

	auto* readBuffer = this->readBuffer;
	this->readBuffer = this->writeBuffer;
	this->writeBuffer = readBuffer;
	this->writeBuffer->clear();

	barrier = new SpinlockBarrier(0, [this]() {
		tickEvent.emit(nullptr, Events::EventArgs());
		
		auto* readBuffer = this->readBuffer;
		this->readBuffer = this->writeBuffer;
		this->writeBuffer = readBuffer;
		this->writeBuffer->clear();
		
		tick++;

		const auto timestamp = std::chrono::high_resolution_clock::now();

		const auto diff = (timestamp - lastCapture).count();
		if (diff > 10e8) {
			currentSpeed = ((tick - lastCaptureTick) * (unsigned long)10e8) / diff;
			lastCapture = timestamp;
			lastCaptureTick = tick;
		}

		if ((unsigned long long)(timestamp - started).count() > this->timeout) {
			currentState = Board::Stopped;
			return;
		}

		if (!--cyclesLeft || currentState == Board::Stopping) {
			currentState = Board::Stopped;
			return;
		}
	}, 2);
}

#endif

unsigned int Board::getNextComponentIndex()
{
	return componentIndex++;
}

unsigned int Board::getThreadCount() const
{
	return threadCount;
}

Component** Board::getComponents() const
{
	return components;
}

Link* Board::getLinks() const
{
	return links;
}

Board::State Board::getCurrentState() const
{
	return currentState;
}

unsigned long long int Board::getCurrentTick() const
{
	return tick;
}

void Board::stop()
{
	if (currentState != Board::Running)
		return;

	currentState = Board::Stopping;
	for (unsigned i = 0; i < threadCount; i++) {
		threads[i]->join();
	}
}

#ifdef __EMSCRIPTEN__

void Board::start(unsigned long long cyclesLeft, unsigned long ms, unsigned int threadCount, const bool synchronized)
{
	if (this->currentState != Board::Stopped)
		return;

	this->currentState = Board::Running;
	this->started = std::chrono::high_resolution_clock::now();
	this->timeout = (unsigned long long)ms * (unsigned long long)10e5;
	this->cyclesLeft = cyclesLeft;

	if (this->cyclesLeft <= 0 || this->timeout <= 0)
	{
		currentState = Board::Stopped;
		return;
	}

	FastStack<Component*> compFlags;
	while (true) {
		for (unsigned long i = 0; i < this->readBuffer->count(); i++) {
			const auto result = std::any_of(this->readBuffer->operator[](i)->outputs, this->readBuffer->operator[](i)->outputs + this->readBuffer->operator[](i)->outputCount, [](Output* x) { return x->getPowered(); });
			if (*this->readBuffer->operator[](i)->powered != result)
			{
				*this->readBuffer->operator[](i)->powered = result;
				for (unsigned int j = 0; j < this->readBuffer->operator[](i)->inputCount; j++)
				{
					compFlags.push(this->readBuffer->operator[](i)->inputs[j]->getComponent());
				}
			}
		}

		while (!compFlags.empty())
		{
			compFlags.pop()->compute();
		}

		tickEvent.emit(nullptr, Events::EventArgs());

		auto* readBuffer = this->readBuffer;
		this->readBuffer = this->writeBuffer;
		this->writeBuffer = readBuffer;
		this->writeBuffer->clear();
		
		tick++;

		const auto timestamp = std::chrono::high_resolution_clock::now();

		const auto diff = (timestamp - lastCapture).count();
		if (diff > 10e8) {
			currentSpeed = ((tick - lastCaptureTick) * (unsigned long)10e8) / diff;
			lastCapture = timestamp;
			lastCaptureTick = tick;
		}

		if ((unsigned long long)(timestamp - started).count() > this->timeout) {
			currentState = Board::Stopped;
			return;
		}

		if (!--cyclesLeft || currentState == Board::Stopping) {
			currentState = Board::Stopped;
			return;
		}
	}
}

#else

void Board::start(const unsigned long long cyclesLeft, const unsigned long ms, const unsigned int threadCount, const bool synchronized)
{
	if (currentState != Board::Stopped)
		return;

	this->started = std::chrono::high_resolution_clock::now();
	this->cyclesLeft = cyclesLeft;
	this->timeout = (unsigned long long)ms * (unsigned long long)10e5;
	this->currentState = Board::Running;

	if (this->cyclesLeft <= 0 || this->timeout <= 0)
	{
		currentState = Board::Stopped;
		return;
	}

	for (unsigned int i = 0; i < this->threadCount; i++)
	{
		if (threads[i] != nullptr)
			delete threads[i];
	}
	delete[] this->threads;
	this->threads = new std::thread*[threadCount] { nullptr };
	this->threadCount = threadCount;
	this->barrier->setStageCount(threadCount);

	for (unsigned int i = 0; i < threadCount; i++) {
		threads[i] = new std::thread([this](const int id) {
			FastStack<Component*> compFlags;
			
			while (true) {
				if (currentState == Board::Stopped)
					return;

				for (unsigned long i = id; i < this->readBuffer->count(); i += this->threadCount) {
					const auto result = std::any_of(this->readBuffer->operator[](i)->outputs, this->readBuffer->operator[](i)->outputs + this->readBuffer->operator[](i)->outputCount, [](Output* x) { return x->getPowered(); });
					if (*this->readBuffer->operator[](i)->powered != result)
					{
						*this->readBuffer->operator[](i)->powered = result;
						for (unsigned int j = 0; j < this->readBuffer->operator[](i)->inputCount; j++)
						{
							compFlags.push(this->readBuffer->operator[](i)->inputs[j]->getComponent());
						}
					}
				}

				barrier->wait();

				while(!compFlags.empty())
				{
					compFlags.pop()->compute();
				}

				barrier->wait();
			}
		}, i);
	}

	if (synchronized)
	{
		for (unsigned i = 0; i < threadCount; i++)
		{
			threads[i]->join();
		}
	}
}

#endif
